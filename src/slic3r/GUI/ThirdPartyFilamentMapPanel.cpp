#include "ThirdPartyFilamentMapPanel.hpp"

#include "AmsMappingPopup.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/PopupWindow.hpp"
#include "format.hpp"

#include <wx/dcclient.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/wrapsizer.h>

#include "Widgets/StateColor.hpp"

#include <algorithm>

namespace Slic3r {
namespace GUI {

namespace {

wxColour parse_slot_colour(const std::string& raw)
{
    std::string s = normalize_map_colour(raw);
    if (s.empty())
        return wxColour(0xCE, 0xCE, 0xCE);
    wxColour c;
    if (c.Set(wxString::FromUTF8(s.c_str())))
        return c;
    return wxColour(0xCE, 0xCE, 0xCE);
}

wxString from_utf8(const std::string& s)
{
    return wxString::FromUTF8(s.c_str());
}

wxString group_title(const ThirdPartySlotGroup& g, bool multiple_nozzles)
{
    wxString title;
    if (multiple_nozzles && g.nozzle_id >= 0) {
        // Bambu AmsMapingPopup: physical 0 is the right nozzle, 1 is the left.
        if (g.nozzle_id == 0)
            title = _L("Right");
        else if (g.nozzle_id == 1)
            title = _L("Left");
        else
            title = format_wxstr(_L("Nozzle %1%"), g.nozzle_id + 1);
    }
    if (g.unit_id >= 0) {
        const wxString unit = format_wxstr(_L("Unit %1%"), g.unit_id + 1);
        title               = title.empty() ? unit : title + " · " + unit;
    }
    return title;
}

} // namespace

class ThirdPartySlotPopup : public PopupWindow
{
public:
    ThirdPartySlotPopup(wxWindow* parent)
        : PopupWindow(parent, wxBORDER_NONE)
    {
        SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
        Bind(wxEVT_PAINT, &ThirdPartySlotPopup::on_paint, this);

        m_sizer = new wxBoxSizer(wxVERTICAL);

        auto* title_panel = new wxPanel(this, wxID_ANY);
        title_panel->SetBackgroundColour(StateColor::darkModeColorFor(wxColour(0xF8, 0xF8, 0xF8)));
        title_panel->SetMinSize(wxSize(-1, FromDIP(30)));
        auto* title_sizer = new wxBoxSizer(wxVERTICAL);
        auto* title       = new wxStaticText(title_panel, wxID_ANY, _L("Device slots"));
        title->SetForegroundColour(StateColor::darkModeColorFor(wxColour(0x32, 0x3A, 0x3D)));
        title->SetFont(::Label::Head_13);
        title_sizer->Add(title, 0, wxALIGN_CENTER | wxALL, FromDIP(6));
        title_panel->SetSizer(title_sizer);

        m_body = new wxBoxSizer(wxVERTICAL);

        m_as_sliced = new MappingItem(this);
        m_as_sliced->SetSize(wxSize(FromDIP(48), FromDIP(60)));
        m_as_sliced->SetMinSize(m_as_sliced->GetSize());
        m_as_sliced->SetMaxSize(m_as_sliced->GetSize());
        TrayData sliced;
        sliced.type = TrayType::EMPTY;
        sliced.colour = wxColour(0xCE, 0xCE, 0xCE);
        m_as_sliced->set_tray_index("-");
        m_as_sliced->set_data(wxColour(0xCE, 0xCE, 0xCE), _L("As sliced"), sliced);
        m_as_sliced->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
            if (m_panel)
                m_panel->pick_slot(m_filament_row, -1);
            Dismiss();
        });

        m_warning = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_warning->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00));
        m_warning->SetFont(::Label::Body_12);
        m_warning->Hide();

        m_sizer->Add(title_panel, 0, wxEXPAND | wxALL, FromDIP(2));
        m_sizer->Add(m_body, 0, wxEXPAND | wxALL, FromDIP(4));
        auto* sliced_row = new wxBoxSizer(wxHORIZONTAL);
        sliced_row->Add(m_as_sliced, 0, wxALIGN_CENTER | wxALL, FromDIP(4));
        m_sizer->Add(sliced_row, 0, wxALIGN_CENTER);
        m_sizer->Add(m_warning, 0, wxEXPAND | wxALL, FromDIP(6));
        SetSizer(m_sizer);
    }

    void set_panel(ThirdPartyFilamentMapPanel* panel) { m_panel = panel; }

    void update(const ThirdPartyFilamentMapModel& model, size_t filament_row)
    {
        m_filament_row = filament_row;
        m_body->Clear(true);
        m_tiles.clear();

        const bool multi = has_multiple_nozzles(model);
        const std::string tag_type = filament_row < model.filaments.size() ? model.filaments[filament_row].type : std::string();

        bool any_type_mismatch = false;
        const auto groups = group_slots(model.slots);
        for (const ThirdPartySlotGroup& g : groups) {
            const wxString title = group_title(g, multi);
            if (!title.empty()) {
                auto* t = new wxStaticText(this, wxID_ANY, title);
                t->SetFont(::Label::Head_13);
                t->SetForegroundColour(StateColor::darkModeColorFor(wxColour(0x32, 0x3A, 0x3D)));
                m_body->Add(t, 0, wxALIGN_CENTER | wxTOP, FromDIP(4));
            }

            for (size_t chunk = 0; chunk < g.slot_positions.size(); chunk += 4) {
                auto* row = new wxBoxSizer(wxHORIZONTAL);
                const size_t n = std::min<size_t>(4, g.slot_positions.size() - chunk);
                auto* container = new MappingContainer(this);
                auto* inner     = new wxBoxSizer(wxHORIZONTAL);
                container->SetSizer(inner);
                inner->Add(0, 0, 0, wxLEFT, FromDIP(6));
                for (size_t k = 0; k < n; ++k) {
                    const int pos = g.slot_positions[chunk + k];
                    const ThirdPartyDeviceSlot& slot = model.slots[pos];
                    MappingItem* item = new MappingItem(container);
                    item->SetSize(wxSize(FromDIP(48), FromDIP(60)));
                    item->SetMinSize(item->GetSize());
                    item->SetMaxSize(item->GetSize());
                    item->m_ams_id  = slot.unit_id;
                    item->m_slot_id = slot.slot_id;

                    TrayData td;
                    td.id            = pos;
                    td.ams_id        = slot.unit_id;
                    td.slot_id       = slot.slot_id;
                    td.filament_type = slot.type;
                    td.name          = slot.type.empty() ? slot.name : slot.type;
                    td.colour        = parse_slot_colour(slot.colour);
                    if (slot.empty)
                        td.type = TrayType::EMPTY;
                    else if (slot.type.empty() && slot.colour.empty())
                        td.type = TrayType::THIRD;
                    else
                        td.type = TrayType::NORMAL;

                    const bool type_unmatch = !slot.empty && !tag_type.empty() && !types_match(tag_type, slot.type);
                    if (type_unmatch)
                        any_type_mismatch = true;

                    const wxString idx = from_utf8(slot_display_label(slot));
                    item->set_tray_index(idx);
                    if (td.type == TrayType::EMPTY)
                        item->set_data(wxColour(0xCE, 0xCE, 0xCE), "-", td);
                    else if (td.type == TrayType::THIRD)
                        item->set_data(wxColour(0xCE, 0xCE, 0xCE), "?", td);
                    else if (type_unmatch)
                        item->set_data(wxColour(0xEE, 0xEE, 0xEE), from_utf8(td.name), td, true);
                    else
                        item->set_data(td.colour, from_utf8(td.name), td);

                    item->Bind(wxEVT_LEFT_DOWN, [this, pos](wxMouseEvent&) {
                        if (m_panel)
                            m_panel->pick_slot(m_filament_row, pos);
                        Dismiss();
                    });
                    inner->Add(item, 0, wxTOP, FromDIP(1));
                    inner->Add(0, 0, 0, wxRIGHT, FromDIP(6));
                    m_tiles.push_back(item);
                }
                container->Show();
                row->Add(container, 0, wxALIGN_CENTER);
                m_body->Add(row, 0, wxALIGN_CENTER | wxALL, FromDIP(2));
            }
        }

        if (any_type_mismatch) {
            m_warning->SetLabel(_L("Note: slots of a different type are greyed. You can still pick them."));
            m_warning->Wrap(FromDIP(248));
            m_warning->Show();
        } else {
            m_warning->Hide();
        }

        Layout();
        Fit();
    }

    void OnDismiss() override
    {
        if (m_panel)
            m_panel->restore_after_popup();
        PopupWindow::OnDismiss();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxPaintDC dc(this);
        dc.SetPen(wxColour(0xAC, 0xAC, 0xAC));
        dc.SetBrush(*wxTRANSPARENT_BRUSH);
        dc.DrawRoundedRectangle(0, 0, GetSize().x, GetSize().y, 0);
    }

    ThirdPartyFilamentMapPanel* m_panel { nullptr };
    size_t                      m_filament_row { 0 };
    wxBoxSizer*                 m_sizer { nullptr };
    wxBoxSizer*                 m_body { nullptr };
    MappingItem*                m_as_sliced { nullptr };
    wxStaticText*               m_warning { nullptr };
    std::vector<MappingItem*>   m_tiles;
};

ThirdPartyFilamentMapPanel::ThirdPartyFilamentMapPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    m_wrap  = new wxWrapSizer(wxHORIZONTAL);
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(m_wrap, 0, wxEXPAND);
    SetSizer(root);

    m_popup = new ThirdPartySlotPopup(this);
    m_popup->set_panel(this);
}

ThirdPartyFilamentMapPanel::~ThirdPartyFilamentMapPanel()
{
    m_popup = nullptr;
}

void ThirdPartyFilamentMapPanel::set_model(ThirdPartyFilamentMapModel model, ChangedFn on_changed)
{
    if (model.mapping.size() != model.filaments.size())
        model.mapping.resize(model.filaments.size(), -1);
    m_model       = std::move(model);
    m_on_changed  = std::move(on_changed);
    rebuild_rows();
}

void ThirdPartyFilamentMapPanel::set_read_only(bool read_only)
{
    if (m_model.read_only == read_only)
        return;
    m_model.read_only = read_only;
    rebuild_rows();
}

void ThirdPartyFilamentMapPanel::set_mapping_enabled(bool enabled)
{
    if (m_mapping_enabled == enabled)
        return;
    m_mapping_enabled = enabled;
    for (MaterialItem* item : m_items) {
        if (!item)
            continue;
        if (enabled)
            item->enable();
        else
            item->disable();
    }
}

std::string ThirdPartyFilamentMapPanel::filament_mapping() const
{
    return encode_filament_mapping(m_model);
}

wxString ThirdPartyFilamentMapPanel::mismatch_warning() const
{
    wxString text;
    auto     add = [&text](const wxString& line) {
        if (!text.IsEmpty())
            text += "\n";
        text += line;
    };
    for (size_t i = 0; i < m_model.filaments.size(); ++i) {
        const ThirdPartyMapMismatch kind = mismatch_at(m_model, i);
        if (kind == ThirdPartyMapMismatch::None)
            continue;
        const int n = m_model.filaments[i].index + 1;
        if (kind == ThirdPartyMapMismatch::Type || kind == ThirdPartyMapMismatch::TypeAndColour) {
            std::string slot_type;
            const int pos = i < m_model.mapping.size() ? m_model.mapping[i] : -1;
            if (pos >= 0 && pos < (int) m_model.slots.size())
                slot_type = m_model.slots[pos].type;
            add(format_wxstr(_L("Filament %1%: its type (%2%) does not match the mapped slot (%3%)."), n,
                             m_model.filaments[i].type, slot_type));
        }
        if (kind == ThirdPartyMapMismatch::Colour || kind == ThirdPartyMapMismatch::TypeAndColour)
            add(format_wxstr(_L("Filament %1%: its colour does not match the mapped slot."), n));
    }
    return text;
}

void ThirdPartyFilamentMapPanel::msw_rescale()
{
    for (MaterialItem* item : m_items)
        if (item)
            item->msw_rescale();
    Layout();
}

void ThirdPartyFilamentMapPanel::sys_color_changed()
{
    SetBackgroundColour(StateColor::darkModeColorFor(*wxWHITE));
    wxGetApp().UpdateDarkUI(this);
    Refresh();
}

void ThirdPartyFilamentMapPanel::rebuild_rows()
{
    m_wrap->Clear(true);
    m_items.clear();
    m_items.resize(m_model.filaments.size(), nullptr);

    for (size_t i = 0; i < m_model.filaments.size(); ++i) {
        const ThirdPartySlicerFilament& f = m_model.filaments[i];
        auto* item = new MaterialItem(this, colour_of(f.colour), material_label(f));
        item->set_dropdown_visible(!m_model.read_only);
        item->set_nozzle_badge(from_utf8(filament_badge_text(f, m_model)));
        if (!m_mapping_enabled)
            item->disable();
        m_items[i] = item;
        apply_slot_to_item(i);
        const size_t row = i;
        item->Bind(wxEVT_LEFT_DOWN, [this, row](wxMouseEvent& e) {
            if (m_model.read_only || !m_mapping_enabled)
                return;
            open_popup(row);
            e.Skip();
        });
        m_wrap->Add(item, 0, wxALL, FromDIP(5));
    }
    Layout();
}

void ThirdPartyFilamentMapPanel::apply_slot_to_item(size_t row)
{
    if (row >= m_items.size() || !m_items[row])
        return;
    MaterialItem* item = m_items[row];
    const int pos = row < m_model.mapping.size() ? m_model.mapping[row] : -1;
    if (pos < 0 || pos >= (int) m_model.slots.size()) {
        item->set_ams_info(wxColour(0xCE, 0xCE, 0xCE), "-");
        item->on_normal();
        return;
    }
    const ThirdPartyDeviceSlot& slot = m_model.slots[pos];
    const wxColour col = slot.empty ? wxColour(0xCE, 0xCE, 0xCE) : colour_of(slot.colour);
    item->set_ams_info(col, from_utf8(slot_display_label(slot)));
    item->on_normal();
    if (mismatch_at(m_model, row) != ThirdPartyMapMismatch::None)
        item->on_warning();
}

void ThirdPartyFilamentMapPanel::open_popup(size_t row)
{
    if (!m_popup || row >= m_items.size() || !m_items[row])
        return;
    MaterialItem* item = m_items[row];
    for (MaterialItem* other : m_items)
        if (other)
            other->on_normal();
    item->on_selected();

    m_popup->update(m_model, row);
    wxPoint pos = item->ClientToScreen(wxPoint(0, 0));
    pos.y += item->GetRect().height;
    m_popup->Move(pos);
    m_popup->Popup();
}

void ThirdPartyFilamentMapPanel::pick_slot(size_t row, int slot_pos)
{
    if (row >= m_model.mapping.size())
        return;
    m_model.mapping[row] = slot_pos;
    restore_after_popup();
    if (m_on_changed)
        m_on_changed();
}

void ThirdPartyFilamentMapPanel::restore_after_popup()
{
    for (size_t i = 0; i < m_items.size(); ++i)
        apply_slot_to_item(i);
}

wxColour ThirdPartyFilamentMapPanel::colour_of(const std::string& raw) const
{
    return parse_slot_colour(raw);
}

wxString ThirdPartyFilamentMapPanel::material_label(const ThirdPartySlicerFilament& f) const
{
    if (!f.type.empty())
        return from_utf8(f.type);
    if (!f.name.empty())
        return from_utf8(f.name);
    return format_wxstr("F%1%", f.index + 1);
}

} // namespace GUI
} // namespace Slic3r
