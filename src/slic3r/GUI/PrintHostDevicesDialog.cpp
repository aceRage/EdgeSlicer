#include "PrintHostDevicesDialog.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "format.hpp"
#include "../Utils/PrintHost.hpp"

namespace Slic3r {
namespace GUI {

using PrintHostDevices::Device;

// The host types a person can pick here: everything the preset's own host_type offers, taken from
// the same option definition so this list never drifts from PrintConfig's.
static void host_type_choices(std::vector<std::string>& keys, wxArrayString& labels)
{
    const ConfigOptionDef* def = print_config_def.get("host_type");
    if (!def)
        return;
    for (size_t i = 0; i < def->enum_values.size(); ++i) {
        keys.push_back(def->enum_values[i]);
        labels.Add(i < def->enum_labels.size() ? _(def->enum_labels[i]) : from_u8(def->enum_values[i]));
    }
}

static wxString host_type_label(const std::string& key)
{
    std::vector<std::string> keys;
    wxArrayString            labels;
    host_type_choices(keys, labels);
    for (size_t i = 0; i < keys.size(); ++i)
        if (keys[i] == key)
            return labels[i];
    return from_u8(key);
}

// ------------------------------------------------------------ the row editor ----

// Alias, address and the credentials of one device. Deliberately the same fields as the
// single-address host editor, so nothing new has to be explained.
class PrintHostDeviceEditDialog : public DPIDialog
{
public:
    PrintHostDeviceEditDialog(wxWindow* parent, const wxString& title, const Device& d)
        : DPIDialog(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_device(d)
    {
        host_type_choices(m_type_keys, m_type_labels);

        auto* grid = new wxFlexGridSizer(2, FromDIP(6), FromDIP(8));
        grid->AddGrowableCol(1, 1);

        auto row = [this, grid](const wxString& label, wxWindow* ctrl) {
            auto* text = new wxStaticText(this, wxID_ANY, label);
            grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(ctrl, 1, wxEXPAND);
        };

        m_alias   = new wxTextCtrl(this, wxID_ANY, from_u8(d.alias), wxDefaultPosition, wxSize(FromDIP(280), -1));
        m_address = new wxTextCtrl(this, wxID_ANY, from_u8(d.address));
        m_type    = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, m_type_labels);
        m_auth    = new wxChoice(this, wxID_ANY);
        m_auth->Append(_L("API key"));
        m_auth->Append(_L("HTTP digest"));
        m_apikey   = new wxTextCtrl(this, wxID_ANY, from_u8(d.apikey));
        m_user     = new wxTextCtrl(this, wxID_ANY, from_u8(d.user));
        m_password = new wxTextCtrl(this, wxID_ANY, from_u8(d.password), wxDefaultPosition, wxDefaultSize, wxTE_PASSWORD);

        int sel = 0;
        for (size_t i = 0; i < m_type_keys.size(); ++i)
            if (m_type_keys[i] == d.host_type)
                sel = (int) i;
        if (!m_type_keys.empty())
            m_type->SetSelection(sel);
        m_auth->SetSelection(d.auth_type == "user" ? 1 : 0);

        row(_L("Name"), m_alias);
        row(_L("Hostname, IP or URL"), m_address);
        row(_L("Host Type"), m_type);
        row(_L("Authorization Type"), m_auth);
        row(_L("API Key / Password"), m_apikey);
        row(_L("User"), m_user);
        row(_L("Password"), m_password);

        auto* top = new wxBoxSizer(wxVERTICAL);
        top->Add(grid, 1, wxEXPAND | wxALL, FromDIP(12));
        if (wxSizer* btns = CreateStdDialogButtonSizer(wxOK | wxCANCEL))
            top->Add(btns, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
        SetSizer(top);
        top->SetSizeHints(this);
        CenterOnParent();

        m_auth->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { update_auth(); });
        update_auth();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    Device device() const
    {
        Device d      = m_device;
        d.alias       = into_u8(m_alias->GetValue());
        d.address     = into_u8(m_address->GetValue());
        const int sel = m_type->GetSelection();
        if (sel >= 0 && sel < (int) m_type_keys.size())
            d.host_type = m_type_keys[sel];
        d.auth_type = m_auth->GetSelection() == 1 ? "user" : "key";
        d.apikey    = into_u8(m_apikey->GetValue());
        d.user      = into_u8(m_user->GetValue());
        d.password  = into_u8(m_password->GetValue());
        return d;
    }

protected:
    void on_dpi_changed(const wxRect&) override { Refresh(); }
    void on_sys_color_changed() override {}

private:
    void update_auth()
    {
        const bool key = m_auth->GetSelection() != 1;
        m_apikey->Enable(key);
        m_user->Enable(!key);
        m_password->Enable(!key);
    }

    Device                   m_device;
    std::vector<std::string> m_type_keys;
    wxArrayString            m_type_labels;
    wxTextCtrl*              m_alias { nullptr };
    wxTextCtrl*              m_address { nullptr };
    wxChoice*                m_type { nullptr };
    wxChoice*                m_auth { nullptr };
    wxTextCtrl*              m_apikey { nullptr };
    wxTextCtrl*              m_user { nullptr };
    wxTextCtrl*              m_password { nullptr };
};

// ----------------------------------------------------------------- the list ----

PrintHostDevicesDialog::PrintHostDevicesDialog(wxWindow* parent, DynamicPrintConfig* config, const std::string& model_key, const wxString& model_label)
    : DPIDialog(parent, wxID_ANY, _L("Devices"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_config(config)
    , m_model_key(model_key)
{
    const int em = GetTextExtent("m").x;

    auto* top = new wxBoxSizer(wxVERTICAL);

    auto* header = new wxStaticText(this, wxID_ANY,
                                    model_label.IsEmpty() ?
                                        _L("The printers of this model, by address.") :
                                        format_wxstr(_L("The printers of %1%, by address. One plate can go to any of them."), model_label));
    top->Add(header, 0, wxEXPAND | wxALL, FromDIP(10));

    m_list = new wxDataViewListCtrl(this, wxID_ANY);
    m_list->AppendTextColumn(_L("Name"), wxDATAVIEW_CELL_INERT, 14 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Hostname, IP or URL"), wxDATAVIEW_CELL_INERT, 18 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("Host Type"), wxDATAVIEW_CELL_INERT, 10 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    // Phase 2 fills this in from the same probe the hub uses; until then it says so rather than
    // pretending a printer is offline.
    m_list->AppendTextColumn(_L("Status"), wxDATAVIEW_CELL_INERT, 10 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    top->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto* btns   = new wxBoxSizer(wxHORIZONTAL);
    m_btn_add    = new wxButton(this, wxID_ANY, _L("Add") + dots);
    m_btn_edit   = new wxButton(this, wxID_ANY, _L("Edit") + dots);
    m_btn_remove = new wxButton(this, wxID_ANY, _L("Remove"));
    m_btn_test   = new wxButton(this, wxID_ANY, _L("Test"));
    auto* btn_close = new wxButton(this, wxID_CANCEL, _L("Close"));
    btns->Add(m_btn_add, 0, wxRIGHT, FromDIP(5));
    btns->Add(m_btn_edit, 0, wxRIGHT, FromDIP(5));
    btns->Add(m_btn_remove, 0, wxRIGHT, FromDIP(5));
    btns->Add(m_btn_test, 0, wxRIGHT, FromDIP(5));
    btns->AddStretchSpacer();
    btns->Add(btn_close, 0);
    top->Add(btns, 0, wxEXPAND | wxALL, FromDIP(10));

    SetSizer(top);
    SetSize(wxSize(60 * em, 28 * em));
    CenterOnParent();

    m_btn_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_add(); });
    m_btn_edit->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_edit(); });
    m_btn_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_remove(); });
    m_btn_test->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { on_test(); });
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { update_buttons(); });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) { on_edit(); });

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(m_list);

    reload();
}

void PrintHostDevicesDialog::reload()
{
    const int keep = m_list->GetSelectedRow();
    m_devices      = PrintHostDevices::devices(m_model_key);
    m_last_used    = PrintHostDevices::last_used_id(m_model_key);
    m_list->DeleteAllItems();
    for (const Device& d : m_devices) {
        wxVector<wxVariant> row;
        wxString            name = from_u8(d.display_name());
        // Not "the current printer" - only where the last plate went, which is what the send
        // dialog will preselect.
        if (!m_last_used.empty() && d.id == m_last_used)
            name += " " + _L("(last used)");
        row.push_back(wxVariant(name));
        row.push_back(wxVariant(from_u8(d.address)));
        row.push_back(wxVariant(host_type_label(d.host_type)));
        row.push_back(wxVariant(_L("unknown")));
        m_list->AppendItem(row);
    }
    if (keep != wxNOT_FOUND && keep < (int) m_devices.size())
        m_list->SelectRow(keep);
    update_buttons();
}

bool PrintHostDevicesDialog::selected(Device& out) const
{
    const int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND || row >= (int) m_devices.size())
        return false;
    out = m_devices[row];
    return true;
}

void PrintHostDevicesDialog::update_buttons()
{
    const bool has = m_list->GetSelectedRow() != wxNOT_FOUND;
    m_btn_edit->Enable(has);
    m_btn_remove->Enable(has);
    m_btn_test->Enable(has);
}

void PrintHostDevicesDialog::on_add()
{
    Device seed;
    // A new device starts from the preset's own host type and model, which is nearly always right.
    if (m_config) {
        const Device from  = PrintHostDevices::from_config(*m_config);
        seed.host_type     = from.host_type;
        seed.printer_model = from.printer_model;
    }
    PrintHostDeviceEditDialog dlg(this, _L("Add a device"), seed);
    if (dlg.ShowModal() != wxID_OK)
        return;
    Device      d = dlg.device();
    std::string error;
    if (!PrintHostDevices::add(m_model_key, d, error)) {
        show_error(this, from_u8(error));
        return;
    }
    reload();
}

void PrintHostDevicesDialog::on_edit()
{
    Device d;
    if (!selected(d))
        return;
    PrintHostDeviceEditDialog dlg(this, _L("Edit the device"), d);
    if (dlg.ShowModal() != wxID_OK)
        return;
    Device      edited = dlg.device();
    std::string error;
    if (!PrintHostDevices::update(m_model_key, edited, error)) {
        show_error(this, from_u8(error));
        return;
    }
    reload();
}

void PrintHostDevicesDialog::on_remove()
{
    Device d;
    if (!selected(d))
        return;
    MessageDialog ask(this, format_wxstr(_L("Remove \"%1%\" from this printer's devices?"), from_u8(d.display_name())), _L("Devices"),
                      wxICON_QUESTION | wxYES_NO);
    if (ask.ShowModal() != wxID_YES)
        return;
    PrintHostDevices::remove(m_model_key, d.id);
    reload();
}

void PrintHostDevicesDialog::on_test()
{
    Device d;
    if (!selected(d))
        return;
    if (!m_config) {
        show_error(this, _L("Could not get a valid Printer Host reference"));
        return;
    }
    // The same test the single-address editor runs, against a copy of the preset's config with this
    // device's address and credentials in it - the preset itself is not touched.
    DynamicPrintConfig cfg = PrintHostDevices::config_for(d, *m_config);
    std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&cfg, false));
    if (!host) {
        show_error(this, _L("Could not get a valid Printer Host reference"));
        return;
    }
    wxString msg;
    bool     result;
    {
        wxBusyCursor wait;
        result = host->test(msg);
    }
    if (result)
        show_info(this, host->get_test_ok_msg(), _L("Success!"));
    else
        show_error(this, host->get_test_failed_msg(msg));
}

void PrintHostDevicesDialog::on_dpi_changed(const wxRect& suggested_rect)
{
    const int em = GetTextExtent("m").x;
    SetSize(wxSize(60 * em, 28 * em));
    Refresh();
}

void show_print_host_devices_dialog(wxWindow* parent, DynamicPrintConfig* config)
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return;
    // First time this model is looked at, the preset's own address becomes device 1.
    try {
        PrintHostDevices::migrate_from_presets(*bundle);
    } catch (...) {}
    const Preset&     preset = bundle->printers.get_edited_preset();
    const std::string key    = PrintHostDevices::model_key_for(preset);
    const std::string label  = PrintHostDevices::from_config(preset.config).printer_model;
    PrintHostDevicesDialog dlg(parent, config ? config : &bundle->printers.get_edited_preset().config, key,
                               from_u8(label.empty() ? preset.name : label));
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
