#include "DualNozzleSliceDialog.hpp"

#include "DeviceManager.hpp"
#include "DualNozzleLayout.hpp"
#include "DualNozzleState.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "PartPlate.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"
#include "Widgets/StaticBox.hpp"

#include "libslic3r/BambuExtruderMap.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/log/trivial.hpp>

#include <wx/bmpcbox.h>
#include <wx/choice.h>
#include <wx/dcbuffer.h>
#include <wx/dcmemory.h>
#include <wx/display.h>
#include <wx/dnd.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <algorithm>
#include <set>

namespace Slic3r { namespace GUI {

using namespace DualNozzleSync;

namespace {

const char *DRAG_PREFIX = "dn_filament:";

// "#RRGGBB" (project) or "RRGGBBAA" (AMS report) -> colour.
wxColour colour_of(const std::string &s)
{
    std::string hex;
    for (char c : s)
        if (c != '#')
            hex.push_back(c);
    if (hex.size() < 6)
        return wxColour(0xCE, 0xCE, 0xCE);
    unsigned long v = 0;
    if (!wxString(hex.substr(0, 6)).ToULong(&v, 16))
        return wxColour(0xCE, 0xCE, 0xCE);
    return wxColour((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

wxBitmap swatch(const wxColour &c, int size)
{
    wxBitmap   bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetBackground(wxBrush(c));
    dc.Clear();
    dc.SetPen(wxPen(wxColour(0x9E, 0x9E, 0x9E)));
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxString tray_label(const Tray &t)
{
    wxString label = from_u8(BambuExtruderMap::tray_name(t.ams_id, t.slot_id));
    if (!t.type.empty())
        label += " " + from_u8(t.type);
    return label;
}

// Colour square with the 1-based filament number; dragging it moves the filament.
class Chip : public wxPanel
{
public:
    Chip(wxWindow *parent, int filament, const wxColour &colour)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, parent->FromDIP(wxSize(26, 26))), m_filament(filament), m_colour(colour)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetCursor(wxCursor(wxCURSOR_HAND));
        SetToolTip(_L("Drag to the other extruder"));
        Bind(wxEVT_PAINT, [this](wxPaintEvent &) {
            wxAutoBufferedPaintDC dc(this);
            dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
            dc.Clear();
            const wxSize sz = GetSize();
            dc.SetPen(wxPen(wxColour("#9E9E9E")));
            dc.SetBrush(wxBrush(m_colour));
            dc.DrawRoundedRectangle(0, 0, sz.x, sz.y, FromDIP(4));
            const double lum = 0.299 * m_colour.Red() + 0.587 * m_colour.Green() + 0.114 * m_colour.Blue();
            dc.SetTextForeground(lum < 160.0 ? *wxWHITE : *wxBLACK);
            dc.SetFont(Label::Body_13);
            const wxString num = wxString::Format("%d", m_filament + 1);
            const wxSize   ext = dc.GetTextExtent(num);
            dc.DrawText(num, (sz.x - ext.x) / 2, (sz.y - ext.y) / 2);
        });
        Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &) {
            wxTextDataObject data(wxString(DRAG_PREFIX) + wxString::Format("%d", m_filament));
            wxDropSource     source(data, GetParent());
            source.DoDragDrop(wxDrag_CopyOnly);
        });
    }

private:
    int      m_filament;
    wxColour m_colour;
};

class SideDropTarget : public wxTextDropTarget
{
public:
    SideDropTarget(DualNozzleSliceDialog *dlg, int side) : m_dlg(dlg), m_side(side) {}
    bool OnDropText(wxCoord, wxCoord, const wxString &text) override
    {
        const wxString prefix(DRAG_PREFIX);
        if (!text.StartsWith(prefix))
            return false;
        long idx = -1;
        if (!text.Mid(prefix.size()).ToLong(&idx))
            return false;
        // Rebuilding the columns destroys the chip being dragged: do it after the drop returns.
        DualNozzleSliceDialog *dlg  = m_dlg;
        const int              side = m_side;
        dlg->CallAfter([dlg, idx, side]() { dlg->move_filament(int(idx), side); });
        return true;
    }

private:
    DualNozzleSliceDialog *m_dlg;
    int                    m_side;
};

double tray_cost(const ProjectFilament &f, const Tray &t)
{
    double cost = color_distance(f.color, t.color);
    std::string a = f.type, b = t.type;
    std::transform(a.begin(), a.end(), a.begin(), ::toupper);
    std::transform(b.begin(), b.end(), b.begin(), ::toupper);
    if (!a.empty() && !b.empty() && a != b)
        cost += 500.;
    return cost;
}

} // namespace

DualNozzleSliceDialog::DualNozzleSliceDialog(wxWindow *parent, PartPlate *plate, int plate_index, ConfirmReason reason)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY,
                wxString::Format(_L("Filament arrangement - Plate %d"), plate_index + 1), wxDefaultPosition, wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_plate(plate)
    , m_plate_index(plate_index)
    , m_reason(reason)
{
    m_used           = DualNozzle::plate_filaments(plate);
    m_filament_count = DualNozzle::project_filament_count();
    m_diameters      = DualNozzle::preset_nozzle_diameters();

    // Starting point: what the plate was confirmed / grouped with before, else the project's map.
    const Confirmation stored = Confirmation::deserialize(plate ? plate->dual_nozzle_confirm() : std::string());
    std::vector<int>   prior  = plate ? plate->get_manual_filament_map() : std::vector<int>();
    const bool         had_arrangement = !prior.empty() || !stored.empty();
    if (prior.empty())
        prior = stored.filament_map;
    if (prior.empty())
        prior = DualNozzle::project_filament_map();

    m_arr.filament_map.assign(m_filament_count, 1);
    for (size_t i = 0; i < m_filament_count && i < prior.size(); ++i)
        m_arr.filament_map[i] = prior[i] == 2 ? 2 : 1;
    m_arr.trays    = stored.trays;
    m_user_touched = had_arrangement;

    build_ui();
    fill_printer_choice();
    read_printer_state(true);
    if (had_arrangement) {
        // Keep the earlier choice; give filaments without a (still loaded) tray the closest one.
        for (const auto &f : m_used) {
            auto it = m_arr.trays.find(f.index);
            if (it == m_arr.trays.end() || !it->second.valid())
                pick_tray_for(f.index, m_arr.filament_map[size_t(f.index)] - 1);
        }
    } else if (m_state.has_report) {
        auto_fill();
        m_user_touched = false;
    }
    rebuild_columns();
    update_status();
    update_issues(); // ends in relayout(): cards and dialog sized to the rows

    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);

    m_timer.SetOwner(this);
    Bind(wxEVT_TIMER, &DualNozzleSliceDialog::on_timer, this, m_timer.GetId());
    m_timer.Start(1000);
}

DualNozzleSliceDialog::~DualNozzleSliceDialog() { m_timer.Stop(); }

void DualNozzleSliceDialog::build_ui()
{
    const wxColour dlg_bg = StateColor::darkModeColorFor(*wxWHITE);
    SetBackgroundColour(dlg_bg);
    SetFont(Label::Body_14);

    auto *v = new wxBoxSizer(wxVERTICAL);

    wxString why;
    switch (m_reason) {
    case ConfirmReason::PrinterChanged: why = _L("A different printer is selected since this plate was last confirmed."); break;
    case ConfirmReason::PrinterStateChanged: why = _L("The printer's spools or nozzles changed since this plate was last confirmed."); break;
    case ConfirmReason::NowSynced: why = _L("This plate was confirmed without printer data; the printer is connected now."); break;
    case ConfirmReason::FilamentsChanged: why = _L("The plate's filaments changed since it was last confirmed."); break;
    case ConfirmReason::MapChanged: why = _L("The plate's grouping changed since it was last confirmed."); break;
    default: why = _L("Check which extruder prints each filament, and from which AMS slot, before slicing."); break;
    }
    auto *intro = new wxStaticText(this, wxID_ANY, why);
    intro->SetBackgroundColour(dlg_bg);
    v->Add(intro, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    // Printer row, as in the send dialog: picker + refresh, plus Sync.
    auto *printer_row = new wxBoxSizer(wxHORIZONTAL);
    auto *printer_lbl = new wxStaticText(this, wxID_ANY, _L("Printer"));
    printer_lbl->SetBackgroundColour(dlg_bg);
    m_printer_choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(260), -1));
    m_refresh_btn    = new Button(this, _L("Refresh"));
    m_sync_btn       = new Button(this, _L("Sync from printer"));
    m_sync_btn->SetToolTip(_L("Read the AMS units, loaded spools and nozzles from the printer and store them in the project"));
    printer_row->Add(printer_lbl, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    printer_row->Add(m_printer_choice, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    printer_row->Add(m_refresh_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    printer_row->Add(m_sync_btn, 0, wxALIGN_CENTER_VERTICAL);
    v->Add(printer_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    m_status = new wxStaticText(this, wxID_ANY, "");
    m_status->SetBackgroundColour(dlg_bg);
    v->Add(m_status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_hint = new wxStaticText(this, wxID_ANY, "");
    m_hint->SetBackgroundColour(dlg_bg);
    m_hint->SetForegroundColour(wxColour(0xFF, 0x6F, 0x00));
    v->Add(m_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    // Left / Right columns.
    auto *cols = new wxBoxSizer(wxHORIZONTAL);
    for (int side = 0; side < 2; ++side) {
        StaticBox *box = new StaticBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(330), -1));
        box->SetCornerRadius(FromDIP(8));
        box->SetBorderWidth(0);
        const wxColour box_bg = StateColor::darkModeColorFor(wxColour("#F4F4F4"));
        box->SetBackgroundColorNormal(box_bg);
        box->SetBackgroundColour(box_bg);
        auto *bs    = new wxBoxSizer(wxVERTICAL);
        auto *title = new wxStaticText(box, wxID_ANY, "");
        title->SetFont(Label::Head_14);
        title->SetBackgroundColour(box_bg);
        bs->Add(title, 0, wxLEFT | wxTOP | wxRIGHT, FromDIP(12));
        // The rows live in a scrolled body. Its height is set by relayout(): every row at its
        // natural height, scrolling only when the dialog would pass 80% of the display. (A
        // fully specified SetMinSize on the card used to pin it at 140 DIP and squash the last
        // row once a side had three or more filaments.)
        auto *body = new wxScrolledWindow(box, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        body->SetBackgroundColour(box_bg);
        body->ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_DEFAULT);
        body->SetScrollRate(0, FromDIP(10));
        auto *rows = new wxBoxSizer(wxVERTICAL);
        body->SetSizer(rows);
        bs->Add(body, 1, wxEXPAND | wxALL, FromDIP(12));
        box->SetSizer(bs);
        box->SetMinSize(wxSize(FromDIP(330), -1)); // width only: the height follows the rows
        box->SetDropTarget(new SideDropTarget(this, side));
        body->SetDropTarget(new SideDropTarget(this, side));
        m_side_box[side]   = box;
        m_side_title[side] = title;
        m_side_rows[side]  = rows;
        m_side_body[side]  = body;
        cols->Add(box, 1, wxEXPAND);
        if (side == 0) {
            auto *mid  = new wxBoxSizer(wxVERTICAL);
            auto *swap = new Button(this, wxString::FromUTF8("\xE2\x87\x84"));
            swap->SetToolTip(_L("Swap the two extruders"));
            swap->SetMinSize(FromDIP(wxSize(36, 28)));
            swap->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { swap_sides(); });
            // Centred against the two cards, which relayout() keeps equally tall.
            mid->AddStretchSpacer();
            mid->Add(swap, 0, wxALIGN_CENTER);
            mid->AddStretchSpacer();
            cols->Add(mid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(8));
        }
    }
    v->Add(cols, 1, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(16));

    auto *tip_row = new wxBoxSizer(wxHORIZONTAL);
    auto *tip     = new wxStaticText(this, wxID_ANY, _L("Drag a filament to the other extruder, or use its arrow. Pick the AMS slot it prints from."));
    tip->SetBackgroundColour(dlg_bg);
    tip->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#6B6B6B")));
    auto *auto_btn = new Button(this, _L("Auto"));
    auto_btn->SetToolTip(_L("Pre-fill from the printer's spools by colour and type (check the result)"));
    auto_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        auto_fill();
        m_user_touched = true;
        rebuild_columns();
        update_issues();
    });
    tip_row->Add(tip, 1, wxALIGN_CENTER_VERTICAL);
    tip_row->Add(auto_btn, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));
    v->Add(tip_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));

    m_issues = new wxStaticText(this, wxID_ANY, "");
    m_issues->SetBackgroundColour(dlg_bg);
    m_issues->SetForegroundColour(wxColour(0xE1, 0x4E, 0x4E));
    v->Add(m_issues, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));

    auto *btns    = new DialogButtons(this, { "Cancel", "Confirm" });
    m_confirm_btn = btns->GetCONFIRM();
    if (m_confirm_btn) {
        m_confirm_btn->SetLabel(_L("Confirm & Slice"));
        m_confirm_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    }
    if (Button *cancel = btns->GetCANCEL())
        cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v->Add(btns, 0, wxEXPAND | wxALL, FromDIP(8));

    m_printer_choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) {
        const int sel = m_printer_choice->GetSelection();
        if (sel < 0 || sel >= (int) m_choice_dev_ids.size())
            return;
        if (DeviceManager *dev = wxGetApp().getDeviceManager()) {
            BOOST_LOG_TRIVIAL(info) << "[DualNozzle] slice dialog selects printer " << m_choice_dev_ids[size_t(sel)];
            dev->set_selected_machine(m_choice_dev_ids[size_t(sel)]);
        }
        read_printer_state(true);
    });
    m_refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        fill_printer_choice();
        read_printer_state(true);
    });
    m_sync_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { sync_now(); });

    SetSizer(v);
}

void DualNozzleSliceDialog::fill_printer_choice()
{
    m_printer_choice->Clear();
    m_choice_dev_ids.clear();
    DeviceManager *dev      = wxGetApp().getDeviceManager();
    MachineObject *selected = dev ? dev->get_selected_machine() : nullptr;
    if (dev) {
        for (const auto &kv : dev->get_my_machine_list()) {
            MachineObject *obj = kv.second;
            if (!obj)
                continue;
            wxString name = from_u8(obj->dev_name.empty() ? obj->dev_id : obj->dev_name);
            if (!obj->is_online())
                name += " " + _L("(offline)");
            m_printer_choice->Append(name);
            m_choice_dev_ids.push_back(obj->dev_id);
            if (selected && selected->dev_id == obj->dev_id)
                m_printer_choice->SetSelection(int(m_choice_dev_ids.size()) - 1);
        }
    }
    if (m_choice_dev_ids.empty()) {
        m_printer_choice->Append(_L("No printer"));
        m_printer_choice->SetSelection(0);
        m_printer_choice->Enable(false);
    } else {
        m_printer_choice->Enable(true);
    }
}

void DualNozzleSliceDialog::read_printer_state(bool refill_if_untouched)
{
    DeviceManager *dev = wxGetApp().getDeviceManager();
    MachineObject *obj = dev ? dev->get_selected_machine() : nullptr;
    DualNozzleSync::PrinterState   s;
    m_model_mismatch = obj && obj->is_multi_extruders() && !DualNozzle::machine_matches_preset(obj);
    if (obj && obj->is_multi_extruders() && !m_model_mismatch) {
        s = DualNozzle::printer_state(obj);
    } else {
        s.physical_extruder_map = DualNozzle::preset_physical_extruder_map();
        if (obj) {
            s.dev_id       = obj->dev_id;
            s.dev_name     = obj->dev_name;
            s.printer_type = obj->printer_type;
        }
    }
    const std::string fp = state_fingerprint(s) + "|" + s.dev_id + (m_model_mismatch ? "|mismatch" : "");
    if (fp == m_state_fp)
        return;
    const bool first_report = !m_state.has_report && s.has_report;
    m_state    = s;
    m_state_fp = fp;

    if (m_state.has_report) {
        // Drop trays that are gone and give those filaments the closest remaining one.
        const auto trays = m_state.all_trays();
        for (const auto &f : m_used) {
            auto it = m_arr.trays.find(f.index);
            if (it == m_arr.trays.end())
                continue;
            const bool present = std::any_of(trays.begin(), trays.end(), [&](const DualNozzleSync::PrinterState::TrayOnSide &t) {
                return t.tray.ams_id == it->second.ams_id && t.tray.slot_id == it->second.slot_id;
            });
            if (!present)
                pick_tray_for(f.index, m_arr.filament_map[size_t(f.index)] - 1);
        }
        if (refill_if_untouched && !m_user_touched && first_report)
            auto_fill();
    }
    if (m_side_rows[0]) {
        rebuild_columns();
        update_status();
        update_issues();
    }
}

void DualNozzleSliceDialog::on_timer(wxTimerEvent &) { read_printer_state(true); }

void DualNozzleSliceDialog::sync_now()
{
    fill_printer_choice();
    m_state_fp.clear(); // force a full re-read
    read_printer_state(false);
    if (m_state.has_report)
        DualNozzle::persist_synced_state(m_state);
    update_status();
}

void DualNozzleSliceDialog::auto_fill()
{
    const Arrangement proposed = propose_arrangement(m_state, m_used, m_filament_count, m_arr.filament_map);
    m_arr.filament_map         = proposed.filament_map;
    for (const auto &f : m_used) {
        auto it = proposed.trays.find(f.index);
        if (it != proposed.trays.end())
            m_arr.trays[f.index] = it->second;
        else
            m_arr.trays.erase(f.index);
    }
}

void DualNozzleSliceDialog::pick_tray_for(int filament, int side)
{
    const auto it_f = std::find_if(m_used.begin(), m_used.end(), [filament](const ProjectFilament &f) { return f.index == filament; });
    if (it_f == m_used.end())
        return;
    std::set<std::pair<int, int>> taken;
    for (const auto &kv : m_arr.trays)
        if (kv.first != filament && kv.second.valid())
            taken.insert({ kv.second.ams_id, kv.second.slot_id });
    const std::vector<Tray> trays = trays_for_extruder(m_state, side);
    const Tray *best = nullptr;
    double      best_cost = 0;
    for (int pass = 0; pass < 2 && !best; ++pass)
        for (const Tray &t : trays) {
            if (pass == 0 && taken.count({ t.ams_id, t.slot_id }))
                continue;
            const double c = tray_cost(*it_f, t);
            if (!best || c < best_cost) {
                best      = &t;
                best_cost = c;
            }
        }
    if (best)
        m_arr.trays[filament] = TrayRef{ best->ams_id, best->slot_id };
    else
        m_arr.trays.erase(filament);
}

void DualNozzleSliceDialog::move_filament(int filament, int side)
{
    if (filament < 0 || size_t(filament) >= m_arr.filament_map.size() || side < 0 || side > 1)
        return;
    if (m_arr.filament_map[size_t(filament)] == side + 1)
        return;
    m_arr.filament_map[size_t(filament)] = side + 1;
    pick_tray_for(filament, side);
    m_user_touched = true;
    rebuild_columns();
    update_issues();
}

void DualNozzleSliceDialog::swap_sides()
{
    for (const auto &f : m_used)
        m_arr.filament_map[size_t(f.index)] = m_arr.filament_map[size_t(f.index)] == 2 ? 1 : 2;
    for (const auto &f : m_used)
        m_arr.trays.erase(f.index);
    for (const auto &f : m_used)
        pick_tray_for(f.index, m_arr.filament_map[size_t(f.index)] - 1);
    m_user_touched = true;
    rebuild_columns();
    update_issues();
}

int DualNozzleSliceDialog::nozzle_count(int side) const
{
    std::vector<std::map<NozzleVolumeType, int>> stats;
    if (m_state.has_report && !m_state.nozzles.empty())
        stats = extruder_nozzle_stats(m_state, 2, m_diameters);
    else if (PresetBundle *pb = wxGetApp().preset_bundle)
        if (const auto *opt = pb->project_config.option<ConfigOptionStrings>("extruder_nozzle_stats"))
            stats = get_extruder_nozzle_stats(opt->values);
    if (size_t(side) >= stats.size())
        return -1;
    int n = 0;
    for (const auto &kv : stats[size_t(side)]) n += kv.second;
    return n;
}

void DualNozzleSliceDialog::rebuild_columns()
{
    Freeze();
    for (int side = 0; side < 2; ++side) {
        m_side_rows[side]->Clear(true);
        const int n = nozzle_count(side);
        wxString  title = side == 0 ? _L("Left extruder") : _L("Right extruder");
        if (n >= 0)
            title += wxString::Format(" (%s)", wxString::Format(n == 1 ? _L("%d nozzle") : _L("%d nozzles"), n));
        m_side_title[side]->SetLabel(title);

        wxScrolledWindow *box    = m_side_body[side];
        const wxColour    box_bg = m_side_box[side]->GetBackgroundColour();
        const auto     trays  = trays_for_extruder(m_state, side);
        int            shown  = 0;
        for (const auto &f : m_used) {
            if (m_arr.filament_map[size_t(f.index)] != side + 1)
                continue;
            ++shown;
            auto *row  = new wxBoxSizer(wxHORIZONTAL);
            auto *chip = new Chip(box, f.index, colour_of(f.color));
            auto *type = new wxStaticText(box, wxID_ANY, from_u8(f.type.empty() ? std::string("-") : f.type), wxDefaultPosition,
                                          wxSize(FromDIP(56), -1));
            type->SetBackgroundColour(box_bg);

            auto *combo = new wxBitmapComboBox(box, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(140), -1), 0, nullptr, wxCB_READONLY);
            const int sw = FromDIP(14);
            combo->Append(m_state.has_report ? _L("No AMS slot") : _L("Printer not synced"), swatch(wxColour(0xE0, 0xE0, 0xE0), sw));
            int selected = 0;
            auto cur = m_arr.trays.find(f.index);
            for (size_t i = 0; i < trays.size(); ++i) {
                combo->Append(tray_label(trays[i]), swatch(colour_of(trays[i].color), sw));
                if (cur != m_arr.trays.end() && cur->second.ams_id == trays[i].ams_id && cur->second.slot_id == trays[i].slot_id)
                    selected = int(i) + 1;
            }
            combo->SetSelection(selected);
            combo->Enable(!trays.empty());
            const int fidx = f.index;
            combo->Bind(wxEVT_COMBOBOX, [this, combo, fidx, trays](wxCommandEvent &) {
                const int sel = combo->GetSelection();
                if (sel <= 0 || size_t(sel - 1) >= trays.size())
                    m_arr.trays.erase(fidx);
                else
                    m_arr.trays[fidx] = TrayRef{ trays[size_t(sel - 1)].ams_id, trays[size_t(sel - 1)].slot_id };
                m_user_touched = true;
                CallAfter([this]() { update_issues(); });
            });

            auto *move = new Button(box, side == 0 ? wxString::FromUTF8("\xE2\x86\x92") : wxString::FromUTF8("\xE2\x86\x90"));
            move->SetToolTip(side == 0 ? _L("Move to the right extruder") : _L("Move to the left extruder"));
            move->SetMinSize(FromDIP(wxSize(28, 24)));
            const int other = 1 - side;
            move->Bind(wxEVT_BUTTON, [this, fidx, other](wxCommandEvent &) { CallAfter([this, fidx, other]() { move_filament(fidx, other); }); });

            if (side == 1)
                row->Add(move, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            row->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            row->Add(type, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            row->Add(combo, 1, wxALIGN_CENTER_VERTICAL);
            if (side == 0)
                row->Add(move, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
            m_side_rows[side]->Add(row, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
        }
        if (shown == 0) {
            auto *empty = new wxStaticText(box, wxID_ANY, _L("No filament - drop one here"));
            empty->SetBackgroundColour(box_bg);
            empty->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#909090")));
            m_side_rows[side]->Add(empty, 0, wxALL, FromDIP(4));
        }
    }
    wxGetApp().UpdateDlgDarkUI(this);
    relayout();
    Thaw();
}

static wxString side_summary(const std::map<int, int> &ams, const std::map<NozzleVolumeType, int> &nozzles, bool nozzles_known)
{
    wxArrayString parts;
    auto ht  = ams.find(1);
    auto ams4 = ams.find(4);
    if (ams4 != ams.end() && ams4->second > 0)
        parts.Add(wxString::Format("%d x AMS", ams4->second));
    if (ht != ams.end() && ht->second > 0)
        parts.Add(wxString::Format("%d x AMS HT", ht->second));
    if (parts.empty())
        parts.Add(_L("no AMS"));
    if (nozzles_known) {
        int      n = 0;
        wxString types;
        for (const auto &kv : nozzles) {
            n += kv.second;
            if (!types.empty()) types += ", ";
            types += from_u8(get_nozzle_volume_type_string(kv.first));
        }
        parts.Add(n == 0 ? _L("no usable nozzle") : wxString::Format(n == 1 ? _L("%d nozzle") : _L("%d nozzles"), n) + " (" + types + ")");
    }
    wxString out;
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? wxString(", ") : wxString()) + parts[i];
    return out;
}

void DualNozzleSliceDialog::update_status()
{
    wxString text;
    bool     warn = true;
    if (m_state.dev_id.empty()) {
        text = _L("No printer is selected. You can slice with this arrangement, but it is not synced with a printer: check each filament's extruder by hand.");
    } else if (m_model_mismatch) {
        text = wxString::Format(_L("%s is a different printer model than this project's printer. Not synced: check the arrangement by hand."),
                                from_u8(m_state.dev_name.empty() ? m_state.dev_id : m_state.dev_name));
    } else if (!m_state.has_report) {
        text = wxString::Format(_L("%s is offline or has not reported yet. Not synced: you can slice, but check the arrangement by hand."),
                                from_u8(m_state.dev_name.empty() ? m_state.dev_id : m_state.dev_name));
    } else {
        warn             = false;
        const auto ams   = extruder_ams_counts(m_state, 2);
        const auto stats = extruder_nozzle_stats(m_state, 2, m_diameters);
        const bool nk    = !m_state.nozzles.empty();
        text = wxString::Format(_L("Synced with %s. Left: %s. Right: %s."), from_u8(m_state.dev_name.empty() ? m_state.dev_id : m_state.dev_name),
                                side_summary(ams[0], stats[0], nk), side_summary(ams[1], stats[1], nk));
    }
    m_status->SetLabel(text);
    m_status->SetForegroundColour(warn ? wxColour(0xFF, 0x6F, 0x00) : StateColor::darkModeColorFor(wxColour("#4A4A4A")));
    m_status->Wrap(FromDIP(700));
    m_sync_btn->Enable(m_state.has_report);
    relayout();
}

void DualNozzleSliceDialog::update_issues()
{
    wxString hint;
    if (all_on_one_side_with_trays_on_both(m_arr, m_state, m_used)) {
        const int side = m_used.empty() ? 1 : m_arr.filament_map[size_t(m_used.front().index)] - 1;
        hint = wxString::Format(_L("Every filament is on the %s extruder, although the printer has spools on both. "
                                   "Automatic grouping can pick the wrong spool when colours are close - check this is what you want."),
                                side == 0 ? _L("left") : _L("right"));
    }
    m_hint->SetLabel(hint);
    m_hint->Wrap(FromDIP(700));
    m_hint->Show(!hint.empty());

    wxArrayString lines;
    bool          blocking = false;
    for (const Issue &is : validate_arrangement(m_arr, m_state, m_used, 2, m_diameters)) {
        const wxString tray = from_u8(BambuExtruderMap::tray_name(is.tray.ams_id, is.tray.slot_id));
        switch (is.kind) {
        case Issue::Kind::TrayOnOtherExtruder:
            lines.Add(wxString::Format(_L("Filament %d: slot %s feeds the other extruder."), is.filament + 1, tray));
            blocking = true;
            break;
        case Issue::Kind::TrayMissing:
            lines.Add(wxString::Format(_L("Filament %d: slot %s is not loaded any more."), is.filament + 1, tray));
            break;
        case Issue::Kind::NoNozzleOnExtruder:
            lines.Add(wxString::Format(_L("The %s extruder has no usable nozzle of this project's diameter."),
                                       is.extruder == 0 ? _L("left") : _L("right")));
            blocking = true;
            break;
        }
    }
    wxString issues_text;
    for (size_t i = 0; i < lines.size(); ++i)
        issues_text += (i ? wxString("\n") : wxString()) + lines[i];
    m_issues->SetLabel(issues_text);
    m_issues->Show(!lines.empty());
    if (m_confirm_btn)
        m_confirm_btn->Enable(!blocking);
    relayout();
}

void DualNozzleSliceDialog::relayout()
{
    if (!m_side_body[0] || !m_side_body[1] || !GetSizer())
        return;
    const int min_body = FromDIP(60);

    // Natural height of each side's rows (the rows sizer's minimum, incl. the per-row gaps).
    int natural[2];
    int rows_w = 0;
    for (int side = 0; side < 2; ++side) {
        const wxSize m = m_side_rows[side]->CalcMin();
        natural[side]  = m.y;
        rows_w         = std::max(rows_w, m.x);
    }
    // Room for the vertical scrollbar is always reserved, so a side starting to scroll neither
    // clips its dropdowns nor makes the two cards different widths.
    int sb_w = wxSystemSettings::GetMetric(wxSYS_VSCROLL_X, this);
    if (sb_w <= 0)
        sb_w = FromDIP(16);
    const int body_w = rows_w + sb_w;

    // The dialog's height without the bodies: measure with both bodies at zero height.
    for (int side = 0; side < 2; ++side)
        m_side_body[side]->SetMinSize(wxSize(body_w, 0));
    SetMinSize(wxDefaultSize);
    const int decoration = GetSize().y - GetClientSize().y;
    const int chrome     = GetSizer()->CalcMin().y + decoration;

    int display_h = 0;
    {
        int idx = wxDisplay::GetFromWindow(this);
        wxDisplay disp(idx == wxNOT_FOUND ? 0u : unsigned(idx));
        display_h = disp.GetClientArea().GetHeight();
    }
    const DualNozzleLayout::CardBodies bodies =
        DualNozzleLayout::card_bodies(natural[0], natural[1], chrome, DualNozzleLayout::max_dialog_height(display_h), min_body);

    for (int side = 0; side < 2; ++side) {
        wxScrolledWindow *body = m_side_body[side];
        body->SetMinSize(wxSize(body_w, bodies.height));
        body->SetMaxSize(wxSize(-1, bodies.height));
        body->FitInside(); // virtual size = the rows' natural size; scrolls when taller than the body
        body->Scroll(0, 0);
        m_side_box[side]->Layout();
    }
    Layout();
    Fit();
    SetMinSize(GetSize());
    BOOST_LOG_TRIVIAL(debug) << "[DualNozzle] arrangement dialog layout: rows " << natural[0] << "/" << natural[1] << " px, chrome " << chrome
                             << ", body " << bodies.height << (bodies.scroll ? " (scrolling)" : "") << ", dialog " << GetSize().y
                             << " of max " << DualNozzleLayout::max_dialog_height(display_h);
}

void DualNozzleSliceDialog::on_dpi_changed(const wxRect &)
{
    rebuild_columns();
    Refresh();
}

}} // namespace Slic3r::GUI
