#include "NozzleStrip.hpp"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/dcgraph.h>
#include <wx/image.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "Label.hpp"
#include "StateColor.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../wxExtensions.hpp"

namespace Slic3r { namespace GUI {

// ---- Sizes (DIP; tests/design_bambu29_mixed_nozzle_compare.md 4.2 / 4.4) ----------------------------------------------
namespace {
constexpr int NS_TILE_MIN_W     = 26;
constexpr int NS_TILE_GAP       = 3;
constexpr int NS_TILE_PAD       = 3;
constexpr int NS_TILE_RADIUS    = 4;
constexpr int NS_ICON_H         = 24;
constexpr int NS_DIVIDER_SPACE  = 7; // gap + 1 DIP line + gap
constexpr int NS_DOT_D          = 4;
constexpr int NS_DOT_GAP        = 2;
constexpr int NS_MAX_DOTS       = 4;
constexpr double NS_DIM_ALPHA   = 0.3;

constexpr int NS_CHIP_H         = 24;
constexpr int NS_CHIP_RADIUS    = 6;
constexpr int NS_CHIP_PAD       = 8;
constexpr int NS_CHIP_GAP       = 6;
constexpr int NS_PILL_PAD       = 3;
constexpr int NS_PILL_RADIUS    = 2;

// 1.5 DIP accent border, at least one pixel.
int strip_border_width(const wxWindow *w) { return std::max(1, int(std::lround(1.5 * w->FromDIP(100) / 100.))); }

wxFont strip_bold_font(wxFont f)
{
    f.MakeBold();
    return f;
}
} // namespace

namespace NozzleStyle {
wxColour tile_background() { return StateColor::darkModeColorFor(wxColour("#F4F4F4")); }
wxColour text() { return StateColor::darkModeColorFor(wxColour("#262E30")); }
wxColour secondary_text() { return StateColor::darkModeColorFor(wxColour("#6B6B6A")); }
wxColour accent() { return StateColor::darkModeColorFor(wxColour("#009688")); }
wxColour divider() { return StateColor::darkModeColorFor(wxColour("#DBDBDB")); }
wxColour error() { return StateColor::darkModeColorFor(wxColour("#D32F2F")); }
wxColour pill_background() { return StateColor::darkModeColorFor(wxColour("#E9E9E9")); }
wxColour hover_background() { return StateColor::darkModeColorFor(wxColour("#E8E8E8")); }
wxColour faded(const wxColour &c, const wxColour &bg, double alpha)
{
    auto mix = [alpha](unsigned char a, unsigned char b) { return (unsigned char) std::lround(a * alpha + b * (1. - alpha)); };
    return wxColour(mix(c.Red(), bg.Red()), mix(c.Green(), bg.Green()), mix(c.Blue(), bg.Blue()));
}
wxFont label_font() { return strip_bold_font(Label::Body_9); }
wxFont size_font() { return Label::Body_9; }
wxFont flow_font() { return Label::Body_8; }
wxFont chip_font() { return strip_bold_font(Label::Body_11); }
wxFont chip_pill_font() { return Label::Body_9; }
} // namespace NozzleStyle

// ---- NozzleTileRow --------------------------------------------------------------------------------------------------

NozzleTileRow::NozzleTileRow(wxWindow *parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    load_icons();
    update_metrics();
    Bind(wxEVT_PAINT, &NozzleTileRow::on_paint, this);
    Bind(wxEVT_SIZE, &NozzleTileRow::on_size, this);
    Bind(wxEVT_MOTION, &NozzleTileRow::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &NozzleTileRow::on_leave, this);
    // On the button press: a release left over from closing a menu or a dialog must not open a tile.
    Bind(wxEVT_LEFT_DOWN, &NozzleTileRow::on_click_event, this);
    Bind(wxEVT_RIGHT_DOWN, &NozzleTileRow::on_click_event, this);
}

void NozzleTileRow::load_icons()
{
    const bool dark = wxGetApp().dark_mode();
    m_icon          = create_scaled_bitmap(dark ? "edge_nozzle_tile_dark" : "edge_nozzle_tile", this, NS_ICON_H);
    wxImage img     = m_icon.ConvertToImage();
    if (!img.HasAlpha())
        img.InitAlpha();
    unsigned char *alpha = img.GetAlpha();
    for (int i = 0, n = img.GetWidth() * img.GetHeight(); alpha != nullptr && i < n; ++i)
        alpha[i] = (unsigned char) std::lround(alpha[i] * NS_DIM_ALPHA);
    m_icon_faded = wxBitmap(img, -1, m_icon.GetScaleFactor());
}

void NozzleTileRow::update_metrics()
{
    wxClientDC dc(this);
    int        widest = 0;
    dc.SetFont(NozzleStyle::label_font());
    widest = std::max(widest, dc.GetTextExtent("R10").x);
    for (const NozzleTileView &t : m_tiles)
        widest = std::max(widest, dc.GetTextExtent(t.label).x);
    dc.SetFont(NozzleStyle::size_font());
    const int size_h = dc.GetTextExtent("0.4").y;
    for (const NozzleTileView &t : m_tiles)
        widest = std::max(widest, dc.GetTextExtent(t.size).x);
    dc.SetFont(NozzleStyle::flow_font());
    const int flow_h = dc.GetTextExtent("Std").y;
    for (const NozzleTileView &t : m_tiles)
        widest = std::max(widest, dc.GetTextExtent(t.flow).x);
    const int icon_h = ScalableBitmap::GetBmpSize(m_icon).y > 0 ? ScalableBitmap::GetBmpSize(m_icon).y : FromDIP(NS_ICON_H);
    // Never narrower than 26 DIP; wider when a label needs it (never clipped).
    m_tile_w = std::max(FromDIP(NS_TILE_MIN_W), widest + FromDIP(2 * NS_TILE_PAD));
    m_tile_h = FromDIP(NS_TILE_PAD) + icon_h + FromDIP(2) + size_h + flow_h + FromDIP(2) + FromDIP(NS_DOT_D) + FromDIP(NS_TILE_PAD);
}

void NozzleTileRow::set_tiles(const std::vector<NozzleTileView> &tiles)
{
    m_tiles = tiles;
    m_hover = -1;
    update_metrics();
    layout_tiles(GetClientSize().x);
    Refresh();
}

wxRect NozzleTileRow::tile_rect(int index) const
{
    if (index < 0 || size_t(index) >= m_rects.size())
        return wxRect();
    wxRect r = m_rects[size_t(index)];
    r.SetPosition(ClientToScreen(r.GetPosition()));
    return r;
}

void NozzleTileRow::layout_tiles(int width)
{
    m_rects.clear();
    m_dividers.clear();
    m_divider_y.clear();
    const int gap  = FromDIP(NS_TILE_GAP);
    const int divw = FromDIP(NS_DIVIDER_SPACE);
    int       x = 0, y = 0;
    for (const NozzleTileView &t : m_tiles) {
        int step = x > 0 ? (t.divider_before ? divw : gap) : 0;
        // Wrap rather than clip: a narrow sidebar gets a second row.
        if (x > 0 && width > 0 && x + step + m_tile_w > width) {
            x = 0;
            y += m_tile_h + gap;
            step = 0;
        }
        if (t.divider_before && step == divw) {
            m_dividers.push_back(x + divw / 2);
            m_divider_y.push_back(y);
        }
        x += step;
        m_rects.emplace_back(x, y, m_tile_w, m_tile_h);
        x += m_tile_w;
    }
    const int height = m_tiles.empty() ? 0 : y + m_tile_h + strip_border_width(this);
    if (height != m_height || GetMinSize().y != height) {
        m_height = height;
        SetMinSize(wxSize(m_tile_w, height));
        InvalidateBestSize();
        if (on_height_changed)
            CallAfter([this]() {
                if (on_height_changed)
                    on_height_changed();
            });
    }
}

wxSize NozzleTileRow::DoGetBestClientSize() const { return wxSize(m_tile_w, m_height > 0 ? m_height : m_tile_h); }

int NozzleTileRow::hit(const wxPoint &pt) const
{
    for (size_t i = 0; i < m_rects.size(); ++i)
        if (m_rects[i].Contains(pt))
            return int(i);
    return -1;
}

void NozzleTileRow::on_size(wxSizeEvent &evt)
{
    evt.Skip();
    layout_tiles(evt.GetSize().x);
    Refresh();
}

void NozzleTileRow::on_motion(wxMouseEvent &evt)
{
    const int h = hit(evt.GetPosition());
    if (h != m_hover) {
        m_hover = h;
        SetToolTip(h >= 0 ? m_tiles[size_t(h)].tooltip : wxString());
        SetCursor(h >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        Refresh();
    }
    evt.Skip();
}

void NozzleTileRow::on_leave(wxMouseEvent &evt)
{
    if (m_hover >= 0) {
        m_hover = -1;
        Refresh();
    }
    evt.Skip();
}

void NozzleTileRow::on_click_event(wxMouseEvent &evt)
{
    const int h = hit(evt.GetPosition());
    if (h >= 0 && on_click) {
        // After the mouse event: the handler opens a menu and may rebuild the strip.
        CallAfter([this, h]() {
            if (on_click)
                on_click(h);
        });
    }
}

void NozzleTileRow::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC pdc(this);
    const wxColour        bg = GetParent()->GetBackgroundColour();
    pdc.SetBackground(wxBrush(bg));
    pdc.Clear();
    wxGCDC dc(pdc);
    for (size_t i = 0; i < m_dividers.size(); ++i) {
        dc.SetPen(wxPen(NozzleStyle::divider(), std::max(1, FromDIP(1))));
        const int top = m_divider_y[i] + m_tile_h / 5;
        dc.DrawLine(m_dividers[i], top, m_dividers[i], m_divider_y[i] + m_tile_h - m_tile_h / 5);
    }
    for (size_t i = 0; i < m_tiles.size() && i < m_rects.size(); ++i)
        paint_tile(dc, int(i), m_rects[i]);
}

void NozzleTileRow::paint_tile(wxDC &dc, int index, const wxRect &rect)
{
    const NozzleTileView &t   = m_tiles[size_t(index)];
    const wxColour        bg  = GetParent()->GetBackgroundColour();
    const double          a   = t.usable ? 1. : NS_DIM_ALPHA;
    auto                  col = [&](const wxColour &c) { return t.usable ? c : NozzleStyle::faded(c, bg, a); };
    const int             bw  = strip_border_width(this);
    wxRect                r   = rect;
    r.Deflate(bw / 2 + 1);

    // Background and border: accent when a filament of the plate prints here, a quiet outline on hover.
    const wxColour fill = col(m_hover == index ? NozzleStyle::hover_background() : NozzleStyle::tile_background());
    if (t.used)
        dc.SetPen(wxPen(col(NozzleStyle::accent()), bw));
    else if (m_hover == index)
        dc.SetPen(wxPen(col(NozzleStyle::secondary_text()), std::max(1, FromDIP(1))));
    else
        dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(fill));
    dc.DrawRoundedRectangle(r, FromDIP(NS_TILE_RADIUS));

    // The hotend icon, its label on a badge over the icon's top.
    int          y      = rect.y + FromDIP(NS_TILE_PAD);
    const wxBitmap &icon = t.usable ? m_icon : m_icon_faded;
    const wxSize icon_sz = ScalableBitmap::GetBmpSize(icon);
    if (icon.IsOk())
        dc.DrawBitmap(icon, rect.x + (rect.width - icon_sz.x) / 2, y, true);
    dc.SetFont(NozzleStyle::label_font());
    const wxSize lsz   = dc.GetTextExtent(t.label);
    const wxRect badge(rect.x + (rect.width - (lsz.x + FromDIP(4))) / 2, y + FromDIP(1), lsz.x + FromDIP(4), lsz.y);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(col(NozzleStyle::tile_background())));
    dc.DrawRoundedRectangle(badge, FromDIP(3));
    dc.SetTextForeground(col(NozzleStyle::text()));
    dc.DrawText(t.label, badge.x + FromDIP(2), badge.y);
    y += (icon_sz.y > 0 ? icon_sz.y : FromDIP(NS_ICON_H)) + FromDIP(2);

    // Size, then flow type.
    dc.SetFont(NozzleStyle::size_font());
    const wxSize ssz = dc.GetTextExtent(t.size);
    dc.SetTextForeground(col(NozzleStyle::text()));
    dc.DrawText(t.size, rect.x + (rect.width - ssz.x) / 2, y);
    y += dc.GetTextExtent("0.4").y;
    dc.SetFont(NozzleStyle::flow_font());
    const wxSize fsz = dc.GetTextExtent(t.flow);
    dc.SetTextForeground(col(NozzleStyle::secondary_text()));
    dc.DrawText(t.flow, rect.x + (rect.width - fsz.x) / 2, y);
    y += dc.GetTextExtent("Std").y + FromDIP(2);

    // Up to four dots in the colours of the filaments printing here; a fourth "+" dot stands for more.
    if (!t.dots.empty()) {
        const int    d     = FromDIP(NS_DOT_D);
        const int    g     = FromDIP(NS_DOT_GAP);
        const size_t shown = std::min<size_t>(t.dots.size(), NS_MAX_DOTS);
        const int    total = int(shown) * d + int(shown - 1) * g;
        int          x     = rect.x + (rect.width - total) / 2;
        for (size_t k = 0; k < shown; ++k, x += d + g) {
            const bool more = k + 1 == NS_MAX_DOTS && t.dots.size() > NS_MAX_DOTS;
            dc.SetPen(wxPen(col(NozzleStyle::divider()), 1));
            dc.SetBrush(wxBrush(more ? col(NozzleStyle::secondary_text()) : col(t.dots[k])));
            dc.DrawEllipse(x, y, d, d);
        }
    }
}

void NozzleTileRow::sys_color_changed()
{
    load_icons();
    Refresh();
}

void NozzleTileRow::msw_rescale()
{
    load_icons();
    update_metrics();
    layout_tiles(GetClientSize().x);
    Refresh();
}

// ---- NozzleStrip ----------------------------------------------------------------------------------------------------

NozzleStrip::NozzleStrip(wxWindow *parent) : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    auto *header = new wxBoxSizer(wxHORIZONTAL);
    m_title      = new wxStaticText(this, wxID_ANY, _L("Nozzles"));
    m_title->SetFont(Label::Body_12);
    m_sync = new ScalableButton(this, wxID_ANY, "nozzle_sync");
    m_sync->SetToolTip(_L("Read the nozzles from the connected printer (the Device page's state; nothing is sent)."));
    m_sync->SetCursor(wxCURSOR_HAND);
    m_sync->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (on_sync)
            CallAfter([this]() { on_sync(); });
    });
    m_settings = new wxStaticText(this, wxID_ANY, _L("Settings") + wxString::FromUTF8(" \xE2\x80\xBA"));
    m_settings->SetFont(Label::Body_12);
    m_settings->SetCursor(wxCURSOR_HAND);
    m_settings->SetToolTip(_L("Extruder settings: the nozzle size and type of every extruder and rack slot in this project."));
    m_settings->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &) {
        if (on_settings)
            CallAfter([this]() { on_settings(); });
    });
    m_settings->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &e) {
        wxFont f = m_settings->GetFont();
        f.SetUnderlined(true);
        m_settings->SetFont(f);
        e.Skip();
    });
    m_settings->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) {
        wxFont f = m_settings->GetFont();
        f.SetUnderlined(false);
        m_settings->SetFont(f);
        e.Skip();
    });
    header->Add(m_title, 0, wxALIGN_CENTER_VERTICAL);
    header->AddStretchSpacer(1);
    header->Add(m_sync, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    header->Add(m_settings, 0, wxALIGN_CENTER_VERTICAL);

    m_row     = new NozzleTileRow(this);
    auto *top = new wxBoxSizer(wxVERTICAL);
    top->Add(header, 0, wxEXPAND | wxBOTTOM, FromDIP(4));
    top->Add(m_row, 0, wxEXPAND | wxBOTTOM, FromDIP(8));
    SetSizer(top);
    sys_color_changed();
}

void NozzleStrip::show_sync(bool show)
{
    if (m_sync->IsShown() != show) {
        m_sync->Show(show);
        Layout();
    }
}

void NozzleStrip::sys_color_changed()
{
    SetBackgroundColour(GetParent()->GetBackgroundColour());
    m_title->SetForegroundColour(NozzleStyle::text());
    m_settings->SetForegroundColour(NozzleStyle::accent());
    m_sync->msw_rescale();
    m_row->sys_color_changed();
    Refresh();
}

void NozzleStrip::msw_rescale()
{
    m_sync->msw_rescale();
    m_row->msw_rescale();
    Layout();
}

// ---- NozzleChipRow --------------------------------------------------------------------------------------------------

NozzleChipRow::NozzleChipRow(wxWindow *parent) : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE | wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &NozzleChipRow::on_paint, this);
    Bind(wxEVT_SIZE, &NozzleChipRow::on_size, this);
    Bind(wxEVT_MOTION, &NozzleChipRow::on_motion, this);
    Bind(wxEVT_LEAVE_WINDOW, &NozzleChipRow::on_leave, this);
    Bind(wxEVT_LEFT_DOWN, &NozzleChipRow::on_click_event, this);
}

static wxString strip_chip_label_text(const NozzleChipView &c) { return c.edited ? c.label + "*" : c.label; }

int NozzleChipRow::chip_width(int index) const
{
    const NozzleChipView &c = m_chips[size_t(index)];
    wxClientDC            dc(const_cast<NozzleChipRow *>(this));
    dc.SetFont(NozzleStyle::chip_font());
    int w = FromDIP(2 * NS_CHIP_PAD) + dc.GetTextExtent(strip_chip_label_text(c)).x;
    if (!c.tools.empty()) {
        dc.SetFont(NozzleStyle::chip_pill_font());
        w += FromDIP(4) + dc.GetTextExtent(c.tools).x + FromDIP(2 * NS_PILL_PAD);
    }
    return w;
}

void NozzleChipRow::set_chips(const std::vector<NozzleChipView> &chips, int selected)
{
    m_chips    = chips;
    m_selected = selected;
    m_hover    = -1;
    layout_chips(GetClientSize().x);
    Refresh();
}

void NozzleChipRow::set_selected(int index)
{
    if (index != m_selected) {
        m_selected = index;
        Refresh();
    }
}

void NozzleChipRow::layout_chips(int width)
{
    m_rects.clear();
    const int h   = FromDIP(NS_CHIP_H);
    const int gap = FromDIP(NS_CHIP_GAP);
    int       x = 0, y = 0, widest = 0;
    for (size_t i = 0; i < m_chips.size(); ++i) {
        const int w = chip_width(int(i));
        widest      = std::max(widest, w);
        // Wrap onto another row instead of becoming a drop-down list.
        if (x > 0 && width > 0 && x + gap + w > width) {
            x = 0;
            y += h + gap;
        } else if (x > 0) {
            x += gap;
        }
        m_rects.emplace_back(x, y, w, h);
        x += w;
    }
    const int height = m_chips.empty() ? 0 : y + h;
    if (height != m_height || GetMinSize().x != widest) {
        m_height = height;
        SetMinSize(wxSize(widest, height));
        InvalidateBestSize();
        if (on_height_changed)
            CallAfter([this]() {
                if (on_height_changed)
                    on_height_changed();
            });
    }
}

wxSize NozzleChipRow::DoGetBestClientSize() const { return wxSize(GetMinSize().x > 0 ? GetMinSize().x : FromDIP(60), m_height > 0 ? m_height : FromDIP(NS_CHIP_H)); }

int NozzleChipRow::hit(const wxPoint &pt) const
{
    for (size_t i = 0; i < m_rects.size(); ++i)
        if (m_rects[i].Contains(pt))
            return int(i);
    return -1;
}

void NozzleChipRow::on_size(wxSizeEvent &evt)
{
    evt.Skip();
    layout_chips(evt.GetSize().x);
    Refresh();
}

void NozzleChipRow::on_motion(wxMouseEvent &evt)
{
    const int h = hit(evt.GetPosition());
    if (h != m_hover) {
        m_hover = h;
        SetToolTip(h >= 0 ? m_chips[size_t(h)].tooltip : wxString());
        SetCursor(h >= 0 ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
        Refresh();
    }
    evt.Skip();
}

void NozzleChipRow::on_leave(wxMouseEvent &evt)
{
    if (m_hover >= 0) {
        m_hover = -1;
        Refresh();
    }
    evt.Skip();
}

void NozzleChipRow::on_click_event(wxMouseEvent &evt)
{
    const int h = hit(evt.GetPosition());
    if (h < 0)
        return;
    m_selected = h;
    Refresh();
    if (on_pick)
        CallAfter([this, h]() {
            if (on_pick)
                on_pick(h);
        });
}

void NozzleChipRow::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC pdc(this);
    const wxColour        bg = GetParent()->GetBackgroundColour();
    pdc.SetBackground(wxBrush(bg));
    pdc.Clear();
    wxGCDC    dc(pdc);
    const int bw = strip_border_width(this);
    for (size_t i = 0; i < m_chips.size() && i < m_rects.size(); ++i) {
        const NozzleChipView &c   = m_chips[i];
        const bool            sel = int(i) == m_selected;
        wxRect                r   = m_rects[i];
        r.Deflate(bw / 2 + 1);
        const wxColour edge = c.missing ? NozzleStyle::error() : sel ? NozzleStyle::accent() : NozzleStyle::divider();
        dc.SetPen(wxPen(edge, sel || c.missing ? bw : std::max(1, FromDIP(1))));
        dc.SetBrush(wxBrush(int(i) == m_hover ? NozzleStyle::hover_background() : bg));
        dc.DrawRoundedRectangle(r, FromDIP(NS_CHIP_RADIUS));
        dc.SetFont(NozzleStyle::chip_font());
        const wxString label = strip_chip_label_text(c);
        const wxSize   lsz   = dc.GetTextExtent(label);
        int            x     = m_rects[i].x + FromDIP(NS_CHIP_PAD);
        dc.SetTextForeground(sel ? NozzleStyle::accent() : NozzleStyle::text());
        dc.DrawText(label, x, m_rects[i].y + (m_rects[i].height - lsz.y) / 2);
        x += lsz.x + FromDIP(4);
        if (!c.tools.empty()) {
            dc.SetFont(NozzleStyle::chip_pill_font());
            const wxSize psz = dc.GetTextExtent(c.tools);
            const wxRect pill(x, m_rects[i].y + (m_rects[i].height - psz.y) / 2 - FromDIP(1), psz.x + FromDIP(2 * NS_PILL_PAD), psz.y + FromDIP(2));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(NozzleStyle::pill_background()));
            dc.DrawRoundedRectangle(pill, FromDIP(NS_PILL_RADIUS));
            dc.SetTextForeground(NozzleStyle::secondary_text());
            dc.DrawText(c.tools, pill.x + FromDIP(NS_PILL_PAD), pill.y + FromDIP(1));
        }
    }
}

void NozzleChipRow::msw_rescale()
{
    layout_chips(GetClientSize().x);
    Refresh();
}

}} // namespace Slic3r::GUI
