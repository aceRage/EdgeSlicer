#include "HomePanel.hpp"

#include "GUI_App.hpp"
#include "HubHomeView.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "WebViewDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/StateColor.hpp"

#include <boost/log/trivial.hpp>

#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

HomePanel::HomePanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    m_sizer = new wxBoxSizer(wxVERTICAL);

    // The strip above the start page, the only way it is shown: it says what this is and leads back.
    m_strip          = new wxPanel(this, wxID_ANY);
    m_strip_label    = new wxStaticText(m_strip, wxID_ANY, _L("Start page"));
    m_strip_label->SetFont(Label::Head_13);
    m_btn_back = new Button(m_strip, _L("Back to the phone hub"));
    m_btn_back->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    auto* strip_sizer = new wxBoxSizer(wxHORIZONTAL);
    strip_sizer->Add(m_strip_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(12));
    strip_sizer->AddStretchSpacer(1);
    strip_sizer->Add(m_btn_back, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM | wxRIGHT, FromDIP(6));
    m_strip->SetSizer(strip_sizer);
    m_strip->Hide();
    m_btn_back->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { show_hub(); });

    m_hub = new HubHomeView(this);
    // Through MainFrame, so its m_webview (what the rest of the app talks to) is the page shown.
    m_hub->on_open_start_page = [this]() {
        if (MainFrame* mf = wxGetApp().mainframe)
            mf->show_start_page();
        else
            show_start_page();
    };

    m_sizer->Add(m_strip, 0, wxEXPAND);
    m_sizer->Add(m_hub, 1, wxEXPAND);
    SetSizer(m_sizer);
    apply_colours();
}

WebViewPanel* HomePanel::start_page()
{
    if (m_start == nullptr) {
        BOOST_LOG_TRIVIAL(info) << "HomePanel: building the start page";
        m_start = new WebViewPanel(this);
        m_start->Hide();
        m_sizer->Add(m_start, 1, wxEXPAND);
        // Keep MainFrame::m_webview in step whoever asked first.
        if (MainFrame* mf = wxGetApp().mainframe)
            if (mf->m_webview == nullptr)
                mf->m_webview = m_start;
    }
    return m_start;
}

void HomePanel::show_start_page()
{
    start_page();
    if (m_start_shown)
        return;
    m_start_shown = true;
    m_hub->Hide();
    m_hub->on_deactivated();
    m_strip->Show();
    m_start->Show();
    Layout();
    if (m_selected)
        notify_start_page(true);
}

void HomePanel::show_hub()
{
    if (!m_start_shown)
        return;
    m_start_shown = false;
    if (m_selected)
        notify_start_page(false);
    if (m_start)
        m_start->Hide();
    m_strip->Hide();
    m_hub->Show();
    Layout();
    if (m_selected)
        m_hub->on_activated();
}

void HomePanel::on_tab_changed(bool selected)
{
    m_selected = selected;
    if (start_page_shown()) {
        notify_start_page(selected);
        return;
    }
    if (selected)
        // After the page switch has settled, so the view already reports itself on screen.
        CallAfter([this]() { m_hub->on_activated(); });
    else
        m_hub->on_deactivated();
}

void HomePanel::notify_start_page(bool active)
{
    if (m_start == nullptr)
        return;
    if (wxWebView* view = m_start->getWebView())
        wxGetApp().page_state_notify_webview(view, active ? "active" : "inactive");
}

void HomePanel::apply_colours()
{
    const wxColour bg    = StateColor::darkModeColorFor(wxColour("#FFFFFF"));
    const wxColour strip = StateColor::darkModeColorFor(wxColour("#F8F8F8"));
    SetBackgroundColour(bg);
    m_strip->SetBackgroundColour(strip);
    m_strip_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour("#262E30")));
    Refresh();
}

void HomePanel::sys_color_changed()
{
    apply_colours();
    m_btn_back->Rescale();
    m_hub->sys_color_changed();
}

} // namespace GUI
} // namespace Slic3r
