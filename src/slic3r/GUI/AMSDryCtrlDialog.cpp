#include "AMSDryCtrlDialog.hpp"

#include "AmsDualLayout.hpp"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>

#include <boost/log/trivial.hpp>

#include <algorithm>

namespace Slic3r { namespace GUI {

using namespace AmsDrying;

namespace {

const wxString &deg()
{
    static const wxString d = wxString::FromUTF8("\xC2\xB0" "C"); // degree sign, kept out of the source encoding
    return d;
}

constexpr auto COMMAND_HOLD = std::chrono::seconds(5); // debounce after Start / Stop

wxString cannot_reason_text(int reason)
{
    switch (CannotDryReason(reason)) {
    case CannotDryReason::InsufficientPower:
        return _L("Insufficient power") + "\n  " +
               _L("Too many AMS drying simultaneously. Please plug in the power or stop other drying processes before starting.");
    case CannotDryReason::AmsBusy:
        return _L("AMS is busy") + "\n  " + _L("AMS is calibrating | reading RFID | loading/unloading material, please wait.");
    case CannotDryReason::ConsumableAtAmsOutlet:
        return _L("Filament in AMS outlet") + "\n  " + _L("The high drying temperature may cause AMS blockage, please unload first.");
    case CannotDryReason::InitiatingAmsDrying: return _L("Initiating AMS drying");
    case CannotDryReason::NotSupportedIn2dMode: return _L("Not supported in 2D mode");
    case CannotDryReason::DryingInProgress: return _L("Task in progress") + "\n  " + _L("The AMS might be in use during Task.");
    case CannotDryReason::Upgrading: return _L("Upgrading") + "\n  " + _L("Firmware update in progress, please wait...");
    case CannotDryReason::InsufficientPowerNeedPluginPower:
        return _L("Insufficient power") + "\n  " + _L("Please plug in the power and then use the drying function.");
    case CannotDryReason::FilamentAtAmsOutletManualUnload:
        return _L("Filament in AMS outlet") + "\n  " +
               _L("The high drying temperature may cause AMS blockage. Please unload the filament manually before proceeding.");
    default: return _L("System is busy") + "\n  " + _L("Initiating other drying processes, please wait a few seconds...");
    }
}

// Bambu's organize_cannot_reasons_text: one of the "busy" reasons, then power, then the rest.
wxString cannot_reasons_text(const std::vector<int> &reasons)
{
    auto has = [&](CannotDryReason r) { return std::find(reasons.begin(), reasons.end(), int(r)) != reasons.end(); };
    wxString text;
    for (CannotDryReason r : {CannotDryReason::DryingInProgress, CannotDryReason::AmsBusy, CannotDryReason::ConsumableAtAmsOutlet,
                              CannotDryReason::FilamentAtAmsOutletManualUnload})
        if (has(r)) {
            text += "* " + cannot_reason_text(int(r)) + "\n";
            break;
        }
    if (has(CannotDryReason::InsufficientPower))
        text += "* " + cannot_reason_text(int(CannotDryReason::InsufficientPower)) + "\n";
    for (int r : reasons)
        if (r == int(CannotDryReason::InsufficientPowerNeedPluginPower) || r == int(CannotDryReason::NotSupportedIn2dMode) ||
            r == int(CannotDryReason::InitiatingAmsDrying) || r == int(CannotDryReason::Upgrading))
            text += "* " + cannot_reason_text(r) + "\n";
    if (text.empty())
        text = "* " + cannot_reason_text(-1) + "\n";
    return text;
}

Ams *find_ams(MachineObject *obj, const std::string &ams_id)
{
    if (!obj)
        return nullptr;
    auto it = obj->amsList.find(ams_id);
    return it == obj->amsList.end() ? nullptr : it->second;
}

} // namespace

AMSDryCtrlDialog::AMSDryCtrlDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("AMS Dryness Control"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    create();
    wxGetApp().UpdateDlgDarkUI(this);
}

void AMSDryCtrlDialog::create()
{
    SetBackgroundColour(*wxWHITE);
    auto *main = new wxBoxSizer(wxHORIZONTAL);

    // Left: picture, status, readouts.
    auto *left = new wxBoxSizer(wxVERTICAL);
    m_image = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    set_image("hum_level1_no_num_light", 96);
    left->Add(m_image, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(20));

    auto *status_row = new wxBoxSizer(wxHORIZONTAL);
    m_status_icon_bmp = ScalableBitmap(this, "ams_is_drying", 20);
    m_status_icon     = new wxStaticBitmap(this, wxID_ANY, m_status_icon_bmp.bmp());
    m_status          = new Label(this, _L("Idle"));
    m_status->SetFont(Label::Head_14);
    status_row->Add(m_status_icon, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
    status_row->Add(m_status, 0, wxALIGN_CENTER_VERTICAL);
    left->Add(status_row, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(12));

    auto *readouts = new wxBoxSizer(wxHORIZONTAL);
    auto  readout  = [this, readouts](const wxString &title, Label *&value, wxWindow **box_out) {
        auto *box = new wxPanel(this);
        box->SetBackgroundColour(GetBackgroundColour());
        auto *col = new wxBoxSizer(wxVERTICAL);
        auto *t   = new Label(box, title);
        t->SetFont(Label::Body_14);
        value = new Label(box, "--");
        value->SetFont(Label::Head_14);
        col->Add(t, 0, wxALIGN_CENTER_HORIZONTAL);
        col->Add(value, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(4));
        box->SetSizer(col);
        readouts->Add(box, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));
        if (box_out)
            *box_out = box;
    };
    readout(_L("Humidity"), m_humidity, nullptr);
    readout(_L("Temperature"), m_temperature, nullptr);
    readout(_L("Remaining Time"), m_remaining, &m_remaining_box);
    left->Add(readouts, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(24));
    main->Add(left, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(16));

    // Right: settings and the button.
    auto *right = new wxBoxSizer(wxVERTICAL);
    m_title     = new Label(this, _L("Filament Drying"));
    m_title->SetFont(Label::Head_14);
    right->Add(m_title, 0, wxTOP, FromDIP(40));

    m_filament = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(180), -1), 0, nullptr, wxCB_READONLY);
    m_types    = preset_types();
    for (const std::string &t : m_types)
        m_filament->Append(wxString::FromUTF8(t));
    m_filament->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &) {
        apply_preset_for_selection();
        revalidate();
    });
    right->Add(m_filament, 0, wxEXPAND | wxTOP, FromDIP(8));

    auto digits_only = [](wxKeyEvent &e) {
        const int k = e.GetKeyCode();
        if ((k >= '0' && k <= '9') || k == WXK_BACK || k == WXK_DELETE || k == WXK_LEFT || k == WXK_RIGHT || k == WXK_TAB || k < WXK_SPACE)
            e.Skip();
    };
    auto *inputs  = new wxBoxSizer(wxHORIZONTAL);
    m_temp_input  = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(64), -1));
    m_temp_input->SetMaxLength(3);
    m_temp_input->Bind(wxEVT_CHAR, digits_only);
    m_temp_input->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { revalidate(); });
    m_hours_input = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(FromDIP(48), -1));
    m_hours_input->SetMaxLength(3);
    m_hours_input->Bind(wxEVT_CHAR, digits_only);
    m_hours_input->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { revalidate(); });
    inputs->Add(m_temp_input, 0, wxALIGN_CENTER_VERTICAL);
    inputs->Add(new Label(this, deg()), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    inputs->AddSpacer(FromDIP(10));
    inputs->Add(m_hours_input, 0, wxALIGN_CENTER_VERTICAL);
    inputs->Add(new Label(this, "H"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    right->Add(inputs, 0, wxTOP, FromDIP(8));

    auto *rotate_row = new wxBoxSizer(wxHORIZONTAL);
    m_rotate         = new ::CheckBox(this);
    m_rotate->SetValue(false);
    m_rotate_label = new Label(this, _L("Rotate spool when drying"));
    m_rotate_label->SetFont(Label::Body_13);
    m_rotate_label->SetToolTip(_L("The AMS will automatically rotate the stored filament slots to enhance the drying performance."));
    rotate_row->Add(m_rotate, 0, wxALIGN_CENTER_VERTICAL);
    rotate_row->Add(m_rotate_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    right->Add(rotate_row, 0, wxTOP, FromDIP(8));

    m_notes = new Label(this, wxEmptyString);
    m_notes->SetFont(Label::Body_12);
    m_notes->SetForegroundColour(wxColour("#FF6F00"));
    right->Add(m_notes, 0, wxEXPAND | wxTOP, FromDIP(6));

    m_cannot = new Label(this, wxEmptyString);
    m_cannot->SetFont(Label::Body_12);
    m_cannot->SetForegroundColour(wxColour("#D01B1B"));
    right->Add(m_cannot, 0, wxEXPAND | wxTOP, FromDIP(6));

    StateColor green_bg(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                        std::pair<wxColour, int>(wxColour(0, 137, 123), StateColor::Pressed),
                        std::pair<wxColour, int>(wxColour(38, 166, 154), StateColor::Hovered),
                        std::pair<wxColour, int>(wxColour(0, 150, 136), StateColor::Normal));
    StateColor red_bg(std::pair<wxColour, int>(wxColour(206, 206, 206), StateColor::Disabled),
                      std::pair<wxColour, int>(wxColour(176, 20, 20), StateColor::Pressed),
                      std::pair<wxColour, int>(wxColour(230, 50, 50), StateColor::Hovered),
                      std::pair<wxColour, int>(wxColour(208, 27, 27), StateColor::Normal));
    auto make_button = [this](const wxString &text, const StateColor &bg) {
        auto *b = new Button(this, text);
        b->SetBackgroundColor(bg);
        b->SetBorderColor(bg);
        b->SetTextColor(StateColor(std::pair<wxColour, int>(*wxWHITE, StateColor::Normal)));
        b->SetFont(Label::Body_14);
        b->SetCornerRadius(FromDIP(12));
        b->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
        return b;
    };
    m_start = make_button(_L("Start"), green_bg);
    m_stop  = make_button(_L("Stop"), red_bg);
    m_start->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_start(); });
    m_stop->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_stop(); });
    right->Add(m_start, 0, wxTOP, FromDIP(12));
    right->Add(m_stop, 0, wxTOP, FromDIP(12));
    m_stop->Hide();
    right->AddStretchSpacer();

    main->Add(right, 0, wxEXPAND | wxRIGHT | wxBOTTOM, FromDIP(20));

    SetSizer(main);
    SetMinSize(wxSize(FromDIP(640), FromDIP(360)));
    Layout();
    Fit();
    CentreOnParent();
}

void AMSDryCtrlDialog::set_image(const std::string &name, int px)
{
    if (name == m_image_name)
        return;
    m_image_name = name;
    m_image_bmp  = ScalableBitmap(this, name, px);
    m_image->SetBitmap(m_image_bmp.bmp());
}

void AMSDryCtrlDialog::set_ams_id(const std::string &ams_id)
{
    if (ams_id != m_ams_id)
        m_list_needs_default = true;
    m_ams_id = ams_id;
}

int AMSDryCtrlDialog::selected_index() const
{
    const int i = m_filament->GetSelection();
    return (i >= 0 && i < int(m_types.size())) ? i : -1;
}

bool AMSDryCtrlDialog::read_inputs(long &temp, long &hours) const
{
    return !m_temp_input->GetValue().IsEmpty() && !m_hours_input->GetValue().IsEmpty() && m_temp_input->GetValue().ToLong(&temp) &&
           m_hours_input->GetValue().ToLong(&hours);
}

void AMSDryCtrlDialog::apply_preset_for_selection()
{
    const int i = selected_index();
    if (i < 0)
        return;
    const Preset *p = preset_for_type(m_types[i]);
    if (!p)
        return;
    m_temp_input->ChangeValue(wxString::Format("%d", preset_temp(*p, m_unit_type, m_printing)));
    m_hours_input->ChangeValue(wxString::Format("%d", preset_hours(*p, m_unit_type, m_printing)));
}

void AMSDryCtrlDialog::revalidate()
{
    Ams *ams = find_ams(m_obj, m_ams_id);
    if (!ams)
        return;
    const bool idle    = is_idle(ams->dry);
    const bool holding = m_command_hold && std::chrono::steady_clock::now() < *m_command_hold;

    wxString notes;
    bool     can_start = false;
    long     temp = 0, hours = 0;
    const int i = selected_index();
    if (idle && i >= 0 && read_inputs(temp, hours)) {
        const Check c = check_request(m_unit_type, m_types[i], int(temp), int(hours), m_printing, m_recommended, m_any_inserted);
        can_start     = c.can_start;
        for (Issue issue : c.issues) {
            switch (issue) {
            case Issue::TempAboveMax:
                notes += wxString::Format(_L("The maximum drying temperature of this AMS is %d") + deg() + ".", c.limits.max_temp) + "\n";
                break;
            case Issue::TempBelowMin:
                notes += wxString::Format(_L("The minimum drying temperature of this AMS is %d") + deg() + ".", c.limits.min_temp) + "\n";
                break;
            case Issue::MayNotFullyDry: notes += _L("This filament may not be completely dried.") + "\n"; break;
            case Issue::AbovePrintingLimit:
                notes += _L("This AMS is currently printing. To ensure print quality, the drying temperature cannot exceed the recommended drying temperature.") +
                         wxString::Format(" (%d", m_recommended) + deg() + ")" + "\n";
                break;
            case Issue::AboveHeatDistortion:
                notes += wxString::Format(_L("The temperature shall not exceed the filament's heat distortion temperature") + " (%d" + deg() + ").", c.heat_distortion) + "\n";
                break;
            case Issue::HoursBelowMin: notes += _L("Minimum time value cannot be less than 1.") + "\n"; break;
            case Issue::HoursAboveMax: notes += wxString::Format(_L("Maximum time value cannot be greater than %d."), c.limits.max_hours) + "\n"; break;
            }
        }
    }
    const bool blocked = ams->dry.has_cannot_reasons && !ams->dry.cannot_reasons.empty() &&
                         !(ams->dry.cannot_reasons.size() == 1 && ams->dry.cannot_reasons[0] == int(CannotDryReason::DryingInProgress));

    m_notes->SetLabel(notes);
    m_notes->Wrap(FromDIP(220));
    m_start->Enable(idle && can_start && !blocked && !holding);
    m_stop->Enable(!holding);
    Layout();
}

void AMSDryCtrlDialog::update(MachineObject *obj)
{
    m_obj    = obj;
    Ams *ams = find_ams(obj, m_ams_id);
    if (!ams || !ams->is_exists || !unit_supports_remote_dry(obj->is_support_remote_dry, ams->type)) {
        BOOST_LOG_TRIVIAL(info) << "AMSDryCtrlDialog: AMS " << m_ams_id << " gone or without remote drying, closing";
        if (IsModal())
            EndModal(wxID_CANCEL);
        else
            Hide();
        return;
    }
    if (m_command_hold && std::chrono::steady_clock::now() >= *m_command_hold)
        m_command_hold.reset();

    m_unit_type = ams->type;
    {
        // "AMS 2 Pro A", "AMS HT B": the model and the letter the slots are labelled with.
        const long n      = std::strtol(m_ams_id.c_str(), nullptr, 10);
        const char letter = char('A' + (n >= 128 ? n - 128 : n) % 26);
        const wxString model = m_unit_type == AmsDual::UNIT_N3S ? wxString("AMS HT") : wxString("AMS 2 Pro");
        SetTitle(_L("AMS Dryness Control") + " - " + model + " " + wxString(letter));
    }
    // "Printing" for drying purposes: a job is running and this unit feeds an extruder.
    bool feeds = obj->m_ams_id == m_ams_id;
    for (const Extder &e : obj->m_extder_data.extders)
        feeds = feeds || (e.snow.ams_id == m_ams_id && AmsDual::slot_is_loaded(e.snow.ams_id, e.snow.slot_id));
    m_printing = obj->is_in_printing() && feeds;

    std::vector<std::string> loaded;
    for (const auto &kv : ams->trayList)
        if (kv.second && kv.second->is_exists)
            loaded.push_back(kv.second->get_filament_type());
    m_any_inserted = !loaded.empty();
    std::string default_type;
    m_recommended = recommended_temp(m_unit_type, loaded, m_printing, &default_type);

    Limits lim;
    limits_for(m_unit_type, lim);
    m_temp_input->SetHint(wxString::Format("%d-%d", lim.min_temp, lim.max_temp) + deg());
    m_hours_input->SetHint(wxString::Format("%d-%d h", lim.min_hours, lim.max_hours));

    // Status and picture.
    const DryState &st   = ams->dry;
    const bool      idle = is_idle(st);
    const bool      dark = wxGetApp().dark_mode();
    int             lvl  = std::clamp(ams->humidity, 1, 5);
    if (idle) {
        set_image("hum_level" + std::to_string(lvl) + (dark ? "_no_num_dark" : "_no_num_light"), 96);
        m_status->SetLabel(_L("Idle"));
        m_status_icon->Hide();
    } else {
        set_image("ams_is_drying", 96);
        wxString s = _L("Drying");
        if (is_error(st))
            s = _L("Drying Error");
        else if (st.sub_status == int(DrySubStatus::Heating))
            s = _L("Drying-Heating");
        else if (st.sub_status == int(DrySubStatus::Dehumidify))
            s = _L("Drying-Dehumidifying");
        else if (st.status == int(DryStatus::Checking))
            s = _L("Checking");
        else if (st.status == int(DryStatus::Stopping))
            s = _L("Stopping");
        m_status->SetLabel(s);
        m_status_icon->Show();
    }

    m_humidity->SetLabel(ams->humidity_raw >= 0 ? wxString::Format("%d%%", ams->humidity_raw) : wxString("--"));
    m_temperature->SetLabel(ams->current_temperature != INVALID_AMS_TEMPERATURE ? wxString::Format("%d", int(ams->current_temperature + 0.5f)) + deg() :
                                                                                  wxString("--"));
    m_remaining->SetLabel(wxString::Format("%02d : %02d", ams->left_dry_time / 60, ams->left_dry_time % 60));
    m_remaining_box->Show(!idle);

    // Controls.
    const bool became_idle = idle && !m_was_idle;
    if (idle) {
        m_filament->Enable();
        m_temp_input->SetEditable(true);
        m_hours_input->SetEditable(true);
        m_rotate->Enable();
        if (m_list_needs_default || became_idle) {
            const auto it = std::find(m_types.begin(), m_types.end(), default_type);
            m_filament->SetSelection(it == m_types.end() ? 0 : int(it - m_types.begin()));
            apply_preset_for_selection();
            m_list_needs_default = false;
        }
    } else {
        m_filament->Disable();
        m_temp_input->SetEditable(false);
        m_hours_input->SetEditable(false);
        m_rotate->Disable();
        if (st.has_settings) {
            const Preset *p = preset_for_type(st.setting_filament);
            if (p) {
                const auto it = std::find(m_types.begin(), m_types.end(), std::string(p->type));
                if (it != m_types.end())
                    m_filament->SetSelection(int(it - m_types.begin()));
            }
            if (st.setting_temp >= 0)
                m_temp_input->ChangeValue(wxString::Format("%d", st.setting_temp));
            if (st.setting_hours >= 0)
                m_hours_input->ChangeValue(wxString::Format("%d", st.setting_hours));
        }
    }
    m_was_idle = idle;

    m_start->Show(idle);
    m_stop->Show(!idle);

    const bool show_reasons = st.has_cannot_reasons && !st.cannot_reasons.empty() &&
                              !(st.cannot_reasons.size() == 1 && st.cannot_reasons[0] == int(CannotDryReason::DryingInProgress));
    if (is_error(st))
        m_cannot->SetLabel(_L("Drying Error") + "\n" + _L("Please check the Assistant for troubleshooting"));
    else if (show_reasons && idle)
        m_cannot->SetLabel(_L("Unable to dry temporarily due to ...") + "\n" + cannot_reasons_text(st.cannot_reasons));
    else
        m_cannot->SetLabel(wxEmptyString);
    m_cannot->Wrap(FromDIP(220));

    revalidate();
    Layout();
    Refresh();
}

void AMSDryCtrlDialog::on_start()
{
    Ams *ams = find_ams(m_obj, m_ams_id);
    const int i = selected_index();
    long temp = 0, hours = 0;
    if (!m_obj || !ams || i < 0 || !read_inputs(temp, hours))
        return;
    const Check c = check_request(m_unit_type, m_types[i], int(temp), int(hours), m_printing, m_recommended, m_any_inserted);
    if (!c.can_start)
        return;
    const std::string &type = m_types[i];
    m_obj->command_ams_filament_drying_start(std::atoi(m_ams_id.c_str()), type, int(temp), int(hours), m_rotate->GetValue(), cooling_temp_for(type));
    m_command_hold = std::chrono::steady_clock::now() + COMMAND_HOLD;
    revalidate();
}

void AMSDryCtrlDialog::on_stop()
{
    if (!m_obj || m_ams_id.empty())
        return;
    m_obj->command_ams_filament_drying_off(std::atoi(m_ams_id.c_str()));
    m_command_hold = std::chrono::steady_clock::now() + COMMAND_HOLD;
    revalidate();
}

void AMSDryCtrlDialog::on_dpi_changed(const wxRect &)
{
    const std::string name = m_image_name;
    m_image_name.clear();
    set_image(name, 96);
    m_status_icon_bmp = ScalableBitmap(this, "ams_is_drying", 20);
    m_status_icon->SetBitmap(m_status_icon_bmp.bmp());
    m_start->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    m_stop->SetMinSize(wxSize(FromDIP(80), FromDIP(28)));
    m_start->Rescale();
    m_stop->Rescale();
    SetMinSize(wxSize(FromDIP(640), FromDIP(360)));
    Layout();
    Fit();
    Refresh();
}

}} // namespace Slic3r::GUI
