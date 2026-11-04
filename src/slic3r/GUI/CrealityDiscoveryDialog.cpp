#include "CrealityDiscoveryDialog.hpp"

#include <atomic>
#include <thread>

#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <boost/log/trivial.hpp>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"
#include "Widgets/Button.hpp"

namespace Slic3r {
namespace GUI {

// What the worker thread and the dialog share. `owner` is only read and written on the UI thread
// (the worker hands its result back through CallAfter), so closing the dialog mid-scan just drops
// the result.
struct CrealityDiscoveryDialog::Session
{
    std::atomic<bool>        cancel { false };
    CrealityDiscoveryDialog* owner { nullptr };
};

CrealityDiscoveryDialog::CrealityDiscoveryDialog(wxWindow* parent, const wxString& ok_label)
    : DPIDialog(parent, wxID_ANY, _L("Find Creality printers"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const int em = GetTextExtent("m").x;
    auto*     top = new wxBoxSizer(wxVERTICAL);

    m_status = new wxStaticText(this, wxID_ANY, wxEmptyString);
    top->Add(m_status, 0, wxEXPAND | wxALL, FromDIP(10));

    m_list = new wxDataViewListCtrl(this, wxID_ANY);
    m_list->AppendTextColumn(_L("Printer"), wxDATAVIEW_CELL_INERT, 20 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("IP"), wxDATAVIEW_CELL_INERT, 12 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->AppendTextColumn(_L("CFS"), wxDATAVIEW_CELL_INERT, 10 * em, wxALIGN_LEFT, wxDATAVIEW_COL_RESIZABLE);
    m_list->SetMinSize(wxSize(46 * em, 14 * em));
    top->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    auto* btns = new wxBoxSizer(wxHORIZONTAL);
    auto  make = [this](const wxString& label, ButtonStyle style) {
        auto* b = new ::Button(this, label);
        b->SetStyle(style, ButtonType::Choice);
        return b;
    };
    m_btn_rescan = make(_L("Rescan"), ButtonStyle::Regular);
    m_btn_ok     = make(ok_label, ButtonStyle::Confirm);
    m_btn_cancel = make(_L("Cancel"), ButtonStyle::Regular);
    m_btn_cancel->SetId(wxID_CANCEL);
    const int gap = FromDIP(ButtonProps::ChoiceButtonGap());
    btns->Add(m_btn_rescan, 0, wxALIGN_CENTER_VERTICAL);
    btns->AddStretchSpacer();
    btns->Add(m_btn_ok, 0, wxRIGHT | wxALIGN_CENTER_VERTICAL, gap);
    btns->Add(m_btn_cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(btns, 0, wxEXPAND | wxALL, FromDIP(10));

    SetSizerAndFit(top);
    CenterOnParent();

    m_btn_rescan->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { start_scan(); });
    m_btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { accept(); });
    m_btn_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    m_list->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent&) { m_btn_ok->Enable(m_list->GetSelectedRow() != wxNOT_FOUND); });
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent&) { accept(); });
    SetEscapeId(wxID_CANCEL);

    wxGetApp().UpdateDlgDarkUI(this);
    wxGetApp().UpdateDVCDarkUI(m_list);

    start_scan();
}

CrealityDiscoveryDialog::~CrealityDiscoveryDialog()
{
    if (m_session) {
        m_session->owner = nullptr;
        m_session->cancel.store(true);
    }
}

void CrealityDiscoveryDialog::start_scan()
{
    if (m_session) {
        m_session->owner = nullptr;
        m_session->cancel.store(true);
    }
    m_session        = std::make_shared<Session>();
    m_session->owner = this;

    m_hosts.clear();
    m_list->DeleteAllItems();
    m_btn_ok->Disable();
    m_btn_rescan->Disable();
    m_status->SetLabel(_L("Looking for Creality printers on this network..."));
    Layout();

    std::shared_ptr<Session> session = m_session;
    std::thread([session]() {
        std::vector<CrealityHost> hosts;
        try {
            hosts = CrealityHostDiscovery::scan(CrealityHostDiscovery::Options(), &session->cancel);
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "CrealityDiscoveryDialog: scan failed: " << e.what();
        }
        if (session->cancel.load() || wxTheApp == nullptr)
            return;
        wxGetApp().CallAfter([session, hosts = std::move(hosts)]() mutable {
            if (session->owner != nullptr)
                session->owner->on_scan_done(std::move(hosts));
        });
    }).detach();
}

void CrealityDiscoveryDialog::on_scan_done(std::vector<CrealityHost> hosts)
{
    m_hosts = std::move(hosts);
    m_list->DeleteAllItems();
    for (const CrealityHost& h : m_hosts) {
        wxVector<wxVariant> row;
        row.push_back(wxVariant(from_u8(h.label())));
        row.push_back(wxVariant(from_u8(h.ip)));
        const std::string cfs = h.cfs_text();
        row.push_back(wxVariant(cfs.empty() ? wxString("-") : (cfs == "No" ? _L("No") : from_u8(cfs))));
        m_list->AppendItem(row);
    }
    m_btn_rescan->Enable();
    if (m_hosts.empty()) {
        m_status->SetLabel(_L("No Creality printers found. Check that the printer is switched on and on this network "
                              "(not a guest Wi-Fi that keeps devices apart), then click Rescan."));
    } else {
        m_status->SetLabel(format_wxstr(_L("Found %1% Creality printer(s). Pick one and click %2%."), m_hosts.size(),
                                        m_btn_ok->GetLabel()));
        m_list->SelectRow(0);
        m_btn_ok->Enable();
    }
    m_status->Wrap(m_list->GetSize().GetWidth());
    Layout();
}

void CrealityDiscoveryDialog::accept()
{
    const int row = m_list->GetSelectedRow();
    if (row == wxNOT_FOUND || row >= int(m_hosts.size()))
        return;
    m_selected = m_hosts[row];
    EndModal(wxID_OK);
}

void CrealityDiscoveryDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
