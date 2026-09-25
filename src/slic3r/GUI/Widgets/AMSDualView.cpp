#include "AMSDualView.hpp"

#include "Label.hpp"
#include "StateColor.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"
#include "../BambuDevicePalette.hpp"

#include <wx/dcbuffer.h>
#include <wx/dcgraph.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

namespace {

// Geometry in DIP, scaled with FromDIP at use.
constexpr int VIEW_W     = 578;
constexpr int VIEW_H     = 318;
constexpr int SIDE_W     = 284;
constexpr int SIDE_GAP   = 10;
constexpr int STRIP_H    = 44;
constexpr int BODY_Y     = STRIP_H + 8;
constexpr int BODY_H     = 190;
constexpr int PILL_H     = 24;
constexpr int LABEL_D    = 24;
constexpr int TILE_Y     = BODY_Y + 66;
constexpr int TILE_W     = 52;
constexpr int TILE_H     = 72;
constexpr int BUS_Y      = TILE_Y + TILE_H + 16;
constexpr int BAND_Y     = BODY_Y + BODY_H + 10;
constexpr int EXT_TOP    = BAND_Y + 6;
constexpr int EXT_IMG_H  = 52; // Bambu's left/right_extruder_*.svg (24 x 62), scaled
// Filament inlets of those pictures, in their 24-wide viewBox: the left half's at x = 14 (its
// right edge sits on the centre line), the right half's at x = 9.8 (its left edge on the centre).
constexpr double EXT_INLET_LEFT  = 14.0 / 24.0;
constexpr double EXT_INLET_RIGHT = 9.8 / 24.0;

bool is_light(const wxColour &c)
{
    if (c.Alpha() == 0)
        return true;
    return (0.299 * c.Red() + 0.587 * c.Green() + 0.114 * c.Blue()) > 150.0;
}

wxColour dm(const char *light_hex) { return StateColor::darkModeColorFor(wxColour(light_hex)); }

} // namespace

AMSDualView::AMSDualView(wxWindow *parent, wxWindowID id)
    : wxWindow(parent, id, wxDefaultPosition, wxDefaultSize, wxCLIP_CHILDREN)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetSize(wxSize(FromDIP(VIEW_W), FromDIP(VIEW_H)));
    SetMinSize(wxSize(FromDIP(VIEW_W), FromDIP(VIEW_H)));
    SetBackgroundColour(palette().page);

    load_bitmaps();

    // Bambu Device page: Bambu Studio's green (BambuDevicePalette.hpp), as the rest of the page.
    StateColor btn_bg_green(std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled),
                            std::pair<wxColour, int>(BambuDevicePalette::GreenPressed, StateColor::Pressed),
                            std::pair<wxColour, int>(BambuDevicePalette::GreenHovered, StateColor::Hovered),
                            std::pair<wxColour, int>(BambuDevicePalette::Green, StateColor::Normal));
    StateColor btn_bg_white(std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled),
                            std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Pressed),
                            std::pair<wxColour, int>(AMS_CONTROL_DEF_BLOCK_BK_COLOUR, StateColor::Hovered),
                            std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Normal));
    StateColor btn_bd_green(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
                            std::pair<wxColour, int>(BambuDevicePalette::Green, StateColor::Enabled));
    StateColor btn_bd_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
                            std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));
    StateColor btn_text_green(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
                              std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Enabled));
    StateColor btn_text_white(std::pair<wxColour, int>(wxColour(255, 255, 254), StateColor::Disabled),
                              std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    auto style = [this](Button *b, const StateColor &bg, const StateColor &bd, const StateColor &fg) {
        b->SetFont(Label::Body_13);
        b->SetBackgroundColor(bg);
        b->SetBorderColor(bd);
        b->SetTextColor(fg);
        b->SetCornerRadius(FromDIP(12));
        b->SetMinSize(wxSize(-1, FromDIP(24)));
    };

    m_btn_auto_refill = new Button(this, _L("Auto-refill"));
    style(m_btn_auto_refill, btn_bg_white, btn_bd_white, btn_text_white);
    m_btn_settings = new wxStaticBitmap(this, wxID_ANY, m_settings_normal.bmp(), wxDefaultPosition, wxSize(FromDIP(24), FromDIP(24)));
    m_btn_settings->SetToolTip(_L("AMS settings"));
    m_btn_settings->Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent &e) { m_btn_settings->SetBitmap(m_settings_hover.bmp()); e.Skip(); });
    m_btn_settings->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &e) { m_btn_settings->SetBitmap(m_settings_normal.bmp()); e.Skip(); });
    m_btn_unload = new Button(this, _L("Unload"));
    style(m_btn_unload, btn_bg_white, btn_bd_white, btn_text_white);
    m_btn_load = new Button(this, _L("Load"));
    style(m_btn_load, btn_bg_green, btn_bd_green, btn_text_green);

    Bind(wxEVT_PAINT, &AMSDualView::on_paint, this);
    Bind(wxEVT_LEFT_DOWN, &AMSDualView::on_left_down, this);
    Bind(wxEVT_MOTION, &AMSDualView::on_motion, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent &e) { layout_controls(); Refresh(); e.Skip(); });

    DualModel empty;
    empty.extruder_count = 2;
    SetModel(empty);
    layout_controls();
}

void AMSDualView::load_bitmaps()
{
    m_pencil_dark  = ScalableBitmap(this, "ams_editable", 14);
    m_pencil_light = ScalableBitmap(this, "ams_editable_light", 14);
    m_sun_idle     = ScalableBitmap(this, "ams_drying", 16);
    m_sun_drying   = ScalableBitmap(this, "ams_is_drying", 16);
    m_hum_num_light.clear();
    m_hum_num_dark.clear();
    m_hum_light.clear();
    m_hum_dark.clear();
    for (int i = 1; i <= 5; ++i) {
        m_hum_num_light.emplace_back(this, "hum_level" + std::to_string(i) + "_light", 16);
        m_hum_num_dark.emplace_back(this, "hum_level" + std::to_string(i) + "_dark", 16);
        m_hum_light.emplace_back(this, "hum_level" + std::to_string(i) + "_no_num_light", 16);
        m_hum_dark.emplace_back(this, "hum_level" + std::to_string(i) + "_no_num_dark", 16);
    }
    m_settings_normal = ScalableBitmap(this, "ams_setting_normal", 24);
    m_settings_hover  = ScalableBitmap(this, "ams_setting_hover", 24);

    // Bambu Studio's unit frames (drawn over the colour cubes, as its AMSPreview does) and
    // extruder halves (its DevExtruderImage).
    m_four_slot        = ScalableBitmap(this, "four_slot_ams_item", 32);
    m_four_slot_dark   = ScalableBitmap(this, "four_slot_ams_item_dark", 32);
    m_single_slot      = ScalableBitmap(this, "single_slot_ams_item", 33);
    m_single_slot_dark = ScalableBitmap(this, "single_slot_ams_item_dark", 33);
    m_ts_cube          = ScalableBitmap(this, "ts_bitmap_cube", 14);
    m_ts_cube_dark     = ScalableBitmap(this, "ts_bitmap_cube_dark", 14);
    for (int active = 0; active < 2; ++active)
        for (int filled = 0; filled < 2; ++filled) {
            const std::string state = std::string(active ? "active" : "unactive") + (filled ? "_filled" : "_empty");
            m_ext_left[active][filled]  = ScalableBitmap(this, "left_extruder_" + state, EXT_IMG_H);
            m_ext_right[active][filled] = ScalableBitmap(this, "right_extruder_" + state, EXT_IMG_H);
        }
}

wxSize AMSDualView::unit_icon_size(const AmsDual::UnitRef &ref) const
{
    if (ref.slot_count > 1)
        return m_four_slot.GetBmpSize();
    return m_single_slot.GetBmpSize();
}

int AMSDualView::extruder_inlet_x(int extruder_id) const
{
    const int mx = GetSize().x / 2;
    if (m_model.extruder_count < 2)
        return mx;
    const int w = m_ext_left[1][1].GetBmpWidth();
    if (extruder_id == AmsDual::DEPUTY_EXTRUDER)
        return mx - w + int(w * EXT_INLET_LEFT + 0.5);
    return mx + int(w * EXT_INLET_RIGHT + 0.5);
}

void AMSDualView::msw_rescale()
{
    load_bitmaps();
    if (m_btn_settings)
        m_btn_settings->SetBitmap(m_settings_normal.bmp());
    for (Button *b : {m_btn_auto_refill, m_btn_unload, m_btn_load})
        if (b) {
            b->SetMinSize(wxSize(-1, FromDIP(24)));
            b->SetCornerRadius(FromDIP(12));
            b->Rescale();
        }
    SetSize(wxSize(FromDIP(VIEW_W), FromDIP(VIEW_H)));
    SetMinSize(wxSize(FromDIP(VIEW_W), FromDIP(VIEW_H)));
    layout_controls();
    Refresh();
}

void AMSDualView::ShowAutoRefill(bool show)
{
    if (m_btn_auto_refill->IsShown() == show)
        return;
    m_btn_auto_refill->Show(show);
    layout_controls();
}

bool AMSDualView::Enable(bool enable)
{
    m_enabled = enable;
    for (wxWindow *w : std::initializer_list<wxWindow *>{m_btn_auto_refill, m_btn_settings, m_btn_unload, m_btn_load})
        if (w)
            w->Enable(enable);
    Refresh();
    return true;
}

AMSDualView::Palette AMSDualView::palette() const
{
    Palette p;
    p.page            = dm("#FFFFFF");
    p.strip           = dm("#EEEEEE");
    p.body            = dm("#F8F8F8");
    p.text            = dm("#323A3D");
    p.text_dim        = dm("#6B6B6B");
    p.line            = dm("#CECECE");
    p.icon_border     = dm("#ACACAC");
    p.icon_bg         = dm("#FFFFFF");
    p.tile_empty      = dm("#FFFFFF");
    p.pill            = dm("#EEEEEE");
    p.brand           = StateColor::darkModeColorFor(BambuDevicePalette::Green);
    p.active_underlay = dm("#6B6B6B");
    return p;
}

int AMSDualView::side_width() const { return FromDIP(SIDE_W); }

int AMSDualView::side_x0(int side) const
{
    const int n     = std::max<int>(1, int(m_sides.size()));
    const int total = n * FromDIP(SIDE_W) + (n - 1) * FromDIP(SIDE_GAP);
    const int x0    = (GetSize().x - total) / 2;
    return x0 + side * (FromDIP(SIDE_W) + FromDIP(SIDE_GAP));
}

const DualUnitView *AMSDualView::unit_view(const std::string &ams_id) const
{
    for (const DualUnitView &u : m_model.units)
        if (u.ref.ams_id == ams_id)
            return &u;
    return nullptr;
}

const DualExtruderView *AMSDualView::extruder_view(int id) const
{
    for (const DualExtruderView &e : m_model.extruders)
        if (e.id == id)
            return &e;
    return nullptr;
}

void AMSDualView::SetModel(const DualModel &model)
{
    m_model = model;
    std::vector<AmsDual::UnitRef> refs;
    for (const DualUnitView &u : m_model.units)
        refs.push_back(u.ref);
    m_sides = AmsDual::group_units(refs, m_model.extruder_count);

    // A selection whose unit or slot went away (unit unplugged, printer switched) is dropped.
    if (!m_sel_ams.empty()) {
        const DualUnitView *u = unit_view(m_sel_ams);
        const bool slot_ok = u && std::any_of(u->slots.begin(), u->slots.end(), [this](const DualSlotView &sv) { return sv.slot_id == m_sel_slot; });
        if (!slot_ok) {
            m_sel_ams.clear();
            m_sel_slot.clear();
        }
    }

    m_page.resize(m_sides.size(), 0);
    m_page_anchor.resize(m_sides.size());
    for (size_t s = 0; s < m_sides.size(); ++s) {
        const AmsDual::Side &side = m_sides[s];
        int page = m_page_anchor[s].empty() ? -1 : AmsDual::page_of_unit(side, m_page_anchor[s]);
        if (page < 0) {
            // First show, or the pinned unit went away: open on the unit that feeds the extruder.
            page = 0;
            if (const DualExtruderView *ext = extruder_view(side.extruder_id); ext && ext->loaded) {
                const int p = AmsDual::page_of_unit(side, ext->ams_id);
                if (p >= 0)
                    page = p;
            }
        }
        if (page >= int(side.pages.size()))
            page = 0;
        m_page[s] = page;
        m_page_anchor[s].clear();
        if (!side.pages.empty() && !side.pages[page].empty())
            m_page_anchor[s] = side.units[side.pages[page].front()].ams_id;
    }
    Refresh();
}


void AMSDualView::Select(const std::string &ams_id, const std::string &slot_id)
{
    m_sel_ams  = ams_id;
    m_sel_slot = slot_id;
    // Bring the selected unit into view on its side.
    const int s = AmsDual::side_of_unit(m_sides, ams_id);
    if (s >= 0) {
        const int p = AmsDual::page_of_unit(m_sides[s], ams_id);
        if (p >= 0) {
            m_page[s]        = p;
            m_page_anchor[s] = ams_id;
        }
    }
    Refresh();
}

std::string AMSDualView::ShownAms() const
{
    int s = AmsDual::side_of_unit(m_sides, m_sel_ams);
    if (s < 0) {
        // Nothing selected yet: the first side that shows a real AMS.
        for (size_t i = 0; i < m_sides.size() && s < 0; ++i)
            for (const AmsDual::UnitRef &u : m_sides[i].units)
                if (u.type != AmsDual::UNIT_EXT_SPOOL) {
                    s = int(i);
                    break;
                }
    }
    if (s < 0 || m_sides[s].pages.empty())
        return m_sel_ams;
    for (int idx : m_sides[s].pages[m_page[s]])
        if (m_sides[s].units[idx].type != AmsDual::UNIT_EXT_SPOOL)
            return m_sides[s].units[idx].ams_id;
    return m_sides[s].units[m_sides[s].pages[m_page[s]].front()].ams_id;
}

wxColour AMSDualView::SlotColour(const std::string &ams_id, const std::string &slot_id) const
{
    if (const DualUnitView *u = unit_view(ams_id))
        for (const DualSlotView &sv : u->slots)
            if (sv.slot_id == slot_id)
                return sv.colour;
    return *wxWHITE;
}

void AMSDualView::layout_controls()
{
    if (!m_btn_load)
        return;
    const wxSize sz    = GetSize();
    const int    left  = side_x0(0);
    const int    right = side_x0(std::max<int>(0, int(m_sides.size()) - 1)) + side_width();
    const int    btn_h = std::max(FromDIP(24), m_btn_load->GetBestSize().y);
    const int    y     = sz.y - btn_h - FromDIP(6);

    wxSize refill = m_btn_auto_refill->GetBestSize();
    refill.y      = btn_h;
    m_btn_auto_refill->SetSize(wxRect(wxPoint(left, y), refill));
    const int settings_x = m_btn_auto_refill->IsShown() ? left + refill.x + FromDIP(10) : left;
    m_btn_settings->SetPosition(wxPoint(settings_x, y + (btn_h - FromDIP(24)) / 2));

    wxSize load = m_btn_load->GetBestSize();
    load.x      = std::max(load.x, FromDIP(72));
    load.y      = btn_h;
    wxSize unload = m_btn_unload->GetBestSize();
    unload.x      = std::max(unload.x, FromDIP(72));
    unload.y      = btn_h;
    m_btn_load->SetSize(wxRect(wxPoint(right - load.x, y), load));
    m_btn_unload->SetSize(wxRect(wxPoint(right - load.x - FromDIP(10) - unload.x, y), unload));
}

void AMSDualView::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC pdc(this);
    wxGCDC                dc(pdc);
    render(dc);
}

void AMSDualView::render(wxDC &dc)
{
    const Palette pal = palette();
    const wxSize  sz  = GetSize();
    if (GetBackgroundColour() != pal.page)
        SetBackgroundColour(pal.page); // the theme was switched
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.page));
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    m_hits.clear();
    for (int s = 0; s < int(m_sides.size()); ++s)
        draw_side(dc, s, pal);
    draw_extruders(dc, pal);
}

void AMSDualView::draw_unit_icon(wxDC &dc, const wxRect &r, const DualUnitView *unit, bool selected, const Palette &pal)
{
    // As Bambu Studio's AMSPreview: colour cubes, then the unit's frame picture on top. The external
    // spool has no frame, just its colour pill on a grey plate.
    const bool dark = wxGetApp().dark_mode();
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(dm("#FFFFFF")));
    dc.DrawRoundedRectangle(r, FromDIP(3));

    if (unit) {
        const bool is_ext = unit->ref.type == AmsDual::UNIT_EXT_SPOOL;
        auto       cube_colour = [&](const DualSlotView &sv, bool &transparent, bool &empty) {
            empty       = sv.state == AMSCanType::AMS_CAN_TYPE_EMPTY || sv.state == AMSCanType::AMS_CAN_TYPE_NONE;
            transparent = !empty && !sv.material.empty() && sv.colour.Alpha() == 0;
            if (empty || sv.material.empty())
                return dm("#FFFFFF");
            return wxColour(sv.colour.Red(), sv.colour.Green(), sv.colour.Blue());
        };

        if (unit->ref.slot_count > 1) {
            const wxSize cube(FromDIP(9), FromDIP(14));
            int          x = r.x + FromDIP(8);
            const int    y = r.y + (r.height - cube.y) / 2;
            for (const DualSlotView &sv : unit->slots) {
                bool transparent = false, empty = false;
                const wxColour c = cube_colour(sv, transparent, empty);
                if (transparent) {
                    const ScalableBitmap &ts = dark ? m_ts_cube_dark : m_ts_cube;
                    dc.DrawBitmap(ts.bmp(), x - FromDIP(1), r.y + (r.height - ts.GetBmpHeight()) / 2, true);
                } else if (!sv.cols.empty() && sv.cols.size() > 1) {
                    const int n = int(sv.cols.size());
                    for (int i = 0; i < n; ++i) {
                        dc.SetBrush(wxBrush(sv.cols[i]));
                        dc.DrawRectangle(x + cube.x * i / n, y, cube.x / n + 1, cube.y);
                    }
                } else {
                    dc.SetBrush(wxBrush(c));
                    dc.DrawRectangle(x, y, cube.x, cube.y);
                    if (empty) {
                        dc.SetPen(wxPen(pal.text, 1));
                        dc.DrawLine(x + cube.x - 1, y + 1, x + 1, y + cube.y - 1);
                        dc.SetPen(*wxTRANSPARENT_PEN);
                    }
                }
                x += cube.x;
            }
            dc.DrawBitmap((dark ? m_four_slot_dark : m_four_slot).bmp(), r.x, r.y, true);
        } else if (!unit->slots.empty()) {
            const DualSlotView &sv = unit->slots.front();
            bool transparent = false, empty = false;
            const wxColour c = cube_colour(sv, transparent, empty);
            // grey plate
            const wxSize plate(FromDIP(16), FromDIP(24));
            dc.SetBrush(wxBrush(dm("#EEEEEE")));
            dc.DrawRoundedRectangle(r.x + (r.width - plate.x) / 2, r.y + (r.height - plate.y) / 2, plate.x, plate.y, FromDIP(2));
            if (!is_ext) {
                const wxSize cube(FromDIP(9), FromDIP(14));
                dc.SetBrush(wxBrush(c));
                dc.DrawRectangle(r.x + (r.width - cube.x) / 2, r.y + (r.height - cube.y) / 2, cube.x, cube.y);
                dc.DrawBitmap((dark ? m_single_slot_dark : m_single_slot).bmp(), r.x, r.y, true);
            } else {
                const wxSize pill(FromDIP(6), FromDIP(12));
                const wxRect pr(r.x + (r.width - pill.x) / 2, r.y + (r.height - pill.y) / 2, pill.x, pill.y);
                dc.SetBrush(wxBrush(c));
                dc.DrawRoundedRectangle(pr, FromDIP(3));
                if (is_light(c) || transparent) {
                    dc.SetPen(wxPen(AMS_CONTROL_GRAY500, 1));
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.DrawRoundedRectangle(pr, FromDIP(3));
                    dc.SetPen(*wxTRANSPARENT_PEN);
                }
            }
        }
    }

    if (selected) {
        wxRect outer = r;
        outer.Inflate(FromDIP(2));
        dc.SetPen(wxPen(pal.brand, FromDIP(2)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(outer, FromDIP(5));
    }
}

void AMSDualView::draw_tile(wxDC &dc, const wxRect &r, const DualSlotView &slot, bool selected, const Palette &pal)
{
    const int  radius  = FromDIP(6);
    const bool empty   = slot.state == AMSCanType::AMS_CAN_TYPE_EMPTY || slot.state == AMSCanType::AMS_CAN_TYPE_NONE;
    const bool unknown = !empty && slot.material.empty();

    wxColour fill = pal.tile_empty;
    if (!empty && !unknown)
        fill = slot.colour.Alpha() == 0 ? pal.tile_empty : wxColour(slot.colour.Red(), slot.colour.Green(), slot.colour.Blue());

    if (selected) {
        dc.SetPen(wxPen(pal.brand, FromDIP(2)));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        wxRect outer = r;
        outer.Inflate(FromDIP(3));
        dc.DrawRoundedRectangle(outer, radius + FromDIP(2));
    }

    dc.SetPen(is_light(fill) || fill == pal.tile_empty ? wxPen(pal.line, 1) : *wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(fill));
    dc.DrawRoundedRectangle(r, radius);

    if (!empty && !unknown && slot.cols.size() > 1) {
        wxRect inner = r;
        inner.Deflate(1);
        dc.SetClippingRegion(inner);
        const int n = int(slot.cols.size());
        for (int i = 0; i < n; ++i) {
            const int x0 = r.x + r.width * i / n, x1 = r.x + r.width * (i + 1) / n;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(slot.cols[i]));
            dc.DrawRectangle(x0, r.y, x1 - x0, r.height);
        }
        dc.DestroyClippingRegion();
        dc.SetPen(wxPen(pal.line, 1));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(r, radius);
    }

    const bool     light_bg = is_light(fill);
    const wxString text     = empty ? _L("Empty") : (unknown ? wxString("?") : slot.material);
    dc.SetFont(unknown ? Label::Head_14 : Label::Body_12);
    if (empty || unknown || fill == pal.tile_empty)
        dc.SetTextForeground(pal.text);
    else
        dc.SetTextForeground(light_bg ? wxColour(0x32, 0x3A, 0x3D) : *wxWHITE);
    wxSize te = dc.GetTextExtent(text);
    if (te.x > r.width - FromDIP(4)) {
        dc.SetFont(Label::Body_10);
        te = dc.GetTextExtent(text);
    }
    dc.DrawText(text, r.x + (r.width - te.x) / 2, r.y + r.height * 2 / 5 - te.y / 2);

    if (!empty && slot.editable) {
        const bool            dark_pencil = (fill == pal.tile_empty) ? !wxGetApp().dark_mode() : light_bg;
        const ScalableBitmap &pencil      = dark_pencil ? m_pencil_dark : m_pencil_light;
        const wxSize          bs          = pencil.GetBmpSize();
        dc.DrawBitmap(pencil.bmp(), r.x + (r.width - bs.x) / 2, r.GetBottom() - bs.y - FromDIP(8), true);
    }
}

void AMSDualView::draw_humidity(wxDC &dc, int side, int cx, int y, const DualUnitView &unit, const Palette &pal)
{
    if (unit.humidity_level < 1 || unit.humidity_level > 5)
        return;
    const bool            dark = wxGetApp().dark_mode();
    const int             lvl  = unit.humidity_level - 1;
    const ScalableBitmap &icon = unit.humidity_percent >= 0 ? (dark ? m_hum_dark[lvl] : m_hum_light[lvl]) :
                                                              (dark ? m_hum_num_dark[lvl] : m_hum_num_light[lvl]);

    dc.SetFont(Label::Body_14);
    const wxString pct   = unit.humidity_percent >= 0 ? wxString::Format("%d", unit.humidity_percent) : wxString();
    const wxSize   pct_e = pct.empty() ? wxSize(0, 0) : dc.GetTextExtent(pct);
    dc.SetFont(Label::Body_12);
    const wxSize sign_e = pct.empty() ? wxSize(0, 0) : dc.GetTextExtent("%");

    const int pad   = FromDIP(8);
    int       width = pad + icon.GetBmpSize().x + (pct.empty() ? 0 : FromDIP(3) + pct_e.x + FromDIP(2) + sign_e.x) + pad;
    if (unit.has_heater)
        width += FromDIP(6) + 1 + FromDIP(6) + m_sun_idle.GetBmpSize().x;
    const int h = FromDIP(PILL_H);
    wxRect    pill(cx - width / 2, y, width, h);

    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.pill));
    dc.DrawRoundedRectangle(pill, h / 2);

    int x = pill.x + pad;
    dc.DrawBitmap(icon.bmp(), x, y + (h - icon.GetBmpSize().y) / 2, true);
    x += icon.GetBmpSize().x;
    if (!pct.empty()) {
        x += FromDIP(3);
        dc.SetFont(Label::Body_14);
        dc.SetTextForeground(pal.text);
        dc.DrawText(pct, x, y + (h - pct_e.y) / 2);
        x += pct_e.x + FromDIP(2);
        dc.SetFont(Label::Body_12);
        dc.DrawText("%", x, y + (h - pct_e.y) / 2 + (pct_e.y - sign_e.y) / 2 + FromDIP(1));
        x += sign_e.x;
    }
    if (unit.has_heater) {
        x += FromDIP(6);
        dc.SetPen(wxPen(pal.icon_border, 1));
        dc.DrawLine(x, y + FromDIP(5), x, y + h - FromDIP(5));
        x += 1 + FromDIP(6);
        const ScalableBitmap &sun = unit.drying ? m_sun_drying : m_sun_idle;
        dc.DrawBitmap(sun.bmp(), x, y + (h - sun.GetBmpSize().y) / 2, true);
    }

    m_hits.push_back(Hit{pill, HitKind::Humidity, side, -1, -1});
}

void AMSDualView::draw_side(wxDC &dc, int s, const Palette &pal)
{
    const AmsDual::Side &side = m_sides[s];
    const int            x0   = side_x0(s);
    const int            w    = side_width();
    const int            cx   = x0 + w / 2;

    // Selector strip.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.strip));
    dc.DrawRoundedRectangle(x0, 0, w, FromDIP(STRIP_H), FromDIP(6));
    static const std::vector<int> no_page;
    const std::vector<int>       &page = side.pages.empty() ? no_page : side.pages[m_page[s]];
    int                           ix   = x0 + FromDIP(10);
    for (int u = 0; u < int(side.units.size()); ++u) {
        const AmsDual::UnitRef &ref     = side.units[u];
        const wxSize            is      = unit_icon_size(ref);
        const int               iw      = is.x;
        const wxRect            r(ix, (FromDIP(STRIP_H) - is.y) / 2, is.x, is.y);
        const bool              on_page = std::find(page.begin(), page.end(), u) != page.end();
        draw_unit_icon(dc, r, unit_view(ref.ams_id), on_page, pal);
        m_hits.push_back(Hit{r, HitKind::UnitIcon, s, u, -1});
        ix += iw + FromDIP(10);
    }

    // Body.
    const int body_y = FromDIP(BODY_Y);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(pal.body));
    dc.DrawRoundedRectangle(x0, body_y, w, FromDIP(BODY_H), FromDIP(8));

    // Tiles on this page, left to right.
    struct TileRef
    {
        int unit;
        int slot;
        int cx;
    };
    std::vector<TileRef> tiles;
    for (int idx : page) {
        const DualUnitView *uv = unit_view(side.units[idx].ams_id);
        const int           n  = uv ? int(uv->slots.size()) : 0;
        for (int k = 0; k < n; ++k)
            tiles.push_back({idx, k, 0});
    }
    const int nt = int(tiles.size());
    for (int i = 0; i < nt; ++i)
        tiles[i].cx = x0 + w * (2 * i + 1) / (2 * nt);

    // Humidity: the first unit on the page that reports one.
    for (int idx : page)
        if (const DualUnitView *uv = unit_view(side.units[idx].ams_id); uv && uv->humidity_level >= 1) {
            draw_humidity(dc, s, cx, body_y + FromDIP(8), *uv, pal);
            break;
        }

    // Feed lines: every tile drops to the bus, the bus joins them at the side's centre, the trunk
    // runs down to the band and across to this side's extruder.
    const DualExtruderView *ext   = extruder_view(side.extruder_id);
    const int               ext_x = extruder_inlet_x(side.extruder_id);
    const int tile_bottom = FromDIP(TILE_Y + TILE_H);
    const int bus_y       = FromDIP(BUS_Y);
    const int band_y      = FromDIP(BAND_Y);
    const int ext_top     = FromDIP(EXT_TOP);
    const int line_w      = std::max(1, FromDIP(2));

    auto draw_path = [&](const wxPen &pen, int from_tile) {
        dc.SetPen(pen);
        if (from_tile >= 0) {
            dc.DrawLine(tiles[from_tile].cx, tile_bottom, tiles[from_tile].cx, bus_y);
            dc.DrawLine(tiles[from_tile].cx, bus_y, cx, bus_y);
        }
        dc.DrawLine(cx, bus_y, cx, band_y);
        dc.DrawLine(cx, band_y, ext_x, band_y);
        dc.DrawLine(ext_x, band_y, ext_x, ext_top);
    };

    wxPen grey(pal.line, line_w);
    if (nt > 0) {
        dc.SetPen(grey);
        for (const TileRef &t : tiles)
            dc.DrawLine(t.cx, tile_bottom, t.cx, bus_y);
        dc.DrawLine(std::min(tiles.front().cx, cx), bus_y, std::max(tiles.back().cx, cx), bus_y);
    }
    draw_path(grey, -1);
    if (page.size() == 1 && nt > 1) { // the hub of a 4-slot unit; paired single slots just meet
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(pal.icon_border));
        dc.DrawRoundedRectangle(cx - FromDIP(10), bus_y - FromDIP(4), FromDIP(20), FromDIP(8), FromDIP(2));
    }

    if (ext && ext->loaded) {
        int from_tile = -1;
        for (int i = 0; i < nt; ++i) {
            const DualUnitView *uv = unit_view(side.units[tiles[i].unit].ams_id);
            if (uv && uv->ref.ams_id == ext->ams_id && tiles[i].slot < int(uv->slots.size()) && uv->slots[tiles[i].slot].slot_id == ext->slot_id)
                from_tile = i;
        }
        const wxColour col = ext->colour.Alpha() == 0 ? pal.tile_empty : wxColour(ext->colour.Red(), ext->colour.Green(), ext->colour.Blue());
        if (is_light(col) || col == pal.page) {
            wxPen under(pal.active_underlay, line_w * 2 + FromDIP(2));
            draw_path(under, from_tile);
        }
        wxPen active(col, line_w * 2);
        draw_path(active, from_tile);
    }

    // Labels and tiles.
    for (int i = 0; i < nt; ++i) {
        const AmsDual::UnitRef &ref = side.units[tiles[i].unit];
        const DualUnitView     *uv  = unit_view(ref.ams_id);
        if (!uv)
            continue;
        const DualSlotView &slot = uv->slots[tiles[i].slot];

        const wxString label = wxString::FromUTF8(AmsDual::slot_label(ref, tiles[i].slot));
        const int      ly    = body_y + FromDIP(PILL_H + 22);
        dc.SetFont(Label::Body_12);
        dc.SetTextForeground(pal.text);
        const wxSize le = dc.GetTextExtent(label);
        if (ref.type == AmsDual::UNIT_EXT_SPOOL) {
            dc.DrawText(label, tiles[i].cx - le.x / 2, ly - le.y / 2);
        } else {
            const int d = FromDIP(LABEL_D);
            dc.SetPen(wxPen(pal.icon_border, 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawCircle(tiles[i].cx, ly, d / 2);
            dc.DrawText(label, tiles[i].cx - le.x / 2, ly - le.y / 2);
            if (uv->rfid_refresh)
                m_hits.push_back(Hit{wxRect(tiles[i].cx - d / 2, ly - d / 2, d, d), HitKind::Label, s, tiles[i].unit, tiles[i].slot});
        }

        const int    tw = FromDIP(TILE_W), th = FromDIP(TILE_H);
        const wxRect tr(tiles[i].cx - tw / 2, FromDIP(TILE_Y), tw, th);
        const bool   selected = ref.ams_id == m_sel_ams && slot.slot_id == m_sel_slot;
        draw_tile(dc, tr, slot, selected, pal);
        m_hits.push_back(Hit{tr, HitKind::Tile, s, tiles[i].unit, tiles[i].slot});
        const bool empty = slot.state == AMSCanType::AMS_CAN_TYPE_EMPTY || slot.state == AMSCanType::AMS_CAN_TYPE_NONE;
        if (!empty && slot.editable) {
            const wxSize bs = m_pencil_dark.GetBmpSize();
            const wxRect pr(tr.x + (tr.width - bs.x) / 2 - FromDIP(4), tr.GetBottom() - bs.y - FromDIP(12), bs.x + FromDIP(8), bs.y + FromDIP(8));
            m_hits.push_back(Hit{pr, HitKind::Pencil, s, tiles[i].unit, tiles[i].slot});
        }
    }

    if (nt == 0) {
        dc.SetFont(Label::Body_13);
        dc.SetTextForeground(pal.text_dim);
        const wxString t  = _L("No AMS");
        const wxSize   te = dc.GetTextExtent(t);
        dc.DrawText(t, cx - te.x / 2, body_y + FromDIP(BODY_H) / 2 - te.y / 2);
    }
}

void AMSDualView::draw_extruders(wxDC &dc, const Palette &pal)
{
    // Bambu Studio's DevExtruderImage: two halves meeting on the centre line, "active" for the
    // extruder in use, "filled" when filament is at the extruder.
    const int mx  = GetSize().x / 2;
    const int top = FromDIP(EXT_TOP);
    if (m_model.extruder_count >= 2) {
        const DualExtruderView *left  = extruder_view(AmsDual::DEPUTY_EXTRUDER);
        const DualExtruderView *right = extruder_view(AmsDual::MAIN_EXTRUDER);
        const bool any_active = (left && left->active) || (right && right->active);
        auto pick = [&](ScalableBitmap (&set)[2][2], const DualExtruderView *ev) -> const ScalableBitmap & {
            const bool active = ev ? (ev->active || !any_active) : true;
            const bool filled = ev && (ev->filled || ev->loaded);
            return set[active ? 1 : 0][filled ? 1 : 0];
        };
        const ScalableBitmap &lb = pick(m_ext_left, left);
        const ScalableBitmap &rb = pick(m_ext_right, right);
        dc.DrawBitmap(lb.bmp(), mx - lb.GetBmpWidth(), top, true);
        dc.DrawBitmap(rb.bmp(), mx, top, true);
    } else {
        const DualExtruderView *ev = extruder_view(AmsDual::MAIN_EXTRUDER);
        const bool filled = ev && (ev->filled || ev->loaded);
        const ScalableBitmap &bmp = m_ext_right[1][filled ? 1 : 0];
        dc.DrawBitmap(bmp.bmp(), mx - int(bmp.GetBmpWidth() * EXT_INLET_RIGHT + 0.5), top, true);
    }
    (void) pal;
}

void AMSDualView::on_left_down(wxMouseEvent &evt)
{
    const wxPoint pos = evt.GetPosition();
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (!it->rect.Contains(pos))
            continue;
        const AmsDual::Side &side = m_sides[it->side];
        switch (it->kind) {
        case HitKind::UnitIcon: {
            const int p = AmsDual::page_of_unit(side, side.units[it->unit].ams_id);
            if (p >= 0) {
                m_page[it->side]        = p;
                m_page_anchor[it->side] = side.units[it->unit].ams_id;
                Refresh();
            }
            return;
        }
        case HitKind::Humidity: {
            if (side.pages.empty())
                return;
            for (int idx : side.pages[m_page[it->side]])
                if (const DualUnitView *uv = unit_view(side.units[idx].ams_id); uv && uv->humidity_level >= 1) {
                    if (on_humidity)
                        on_humidity(uv->ref.ams_id, ClientToScreen(it->rect.GetBottomLeft()));
                    break;
                }
            return;
        }
        case HitKind::Tile:
        case HitKind::Pencil:
        case HitKind::Label: {
            if (!m_enabled)
                return;
            const DualUnitView *uv = unit_view(side.units[it->unit].ams_id);
            if (!uv || it->slot < 0 || it->slot >= int(uv->slots.size()))
                return;
            const std::string ams     = uv->ref.ams_id;
            const std::string slot    = uv->slots[it->slot].slot_id;
            const bool        changed = ams != m_sel_ams || slot != m_sel_slot;
            m_sel_ams                 = ams;
            m_sel_slot                = slot;
            Refresh();
            if (changed && on_selection_changed)
                on_selection_changed();
            if (it->kind == HitKind::Pencil && on_edit)
                on_edit(ams, slot);
            else if (it->kind == HitKind::Label && on_refresh)
                on_refresh(ams, slot);
            return;
        }
        }
    }
    evt.Skip();
}

void AMSDualView::on_motion(wxMouseEvent &evt)
{
    bool hand = false;
    for (const Hit &h : m_hits)
        if (h.rect.Contains(evt.GetPosition()) && (m_enabled || h.kind == HitKind::UnitIcon || h.kind == HitKind::Humidity)) {
            hand = true;
            break;
        }
    SetCursor(hand ? wxCursor(wxCURSOR_HAND) : wxCursor(wxCURSOR_ARROW));
    evt.Skip();
}

}} // namespace Slic3r::GUI
