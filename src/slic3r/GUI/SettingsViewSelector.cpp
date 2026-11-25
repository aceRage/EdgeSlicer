#include "SettingsViewSelector.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/SegmentedToggle.hpp"
#include "Widgets/StateColor.hpp"

#include "libslic3r/SettingsView.hpp"

#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

namespace {

// Segment order: index == int(ViewChoice) for Basic, Advanced and Bambu.
constexpr int settings_view_segment_min_width_dip = 56;
constexpr int settings_view_combo_width_dip       = 96;

wxString settings_view_segment_label(int index)
{
    switch (index) {
    case 0: return _L("Basic");
    case 1: return _L("Advanced");
    default: return "Bambu"; // a name, not translated
    }
}

wxString settings_view_segment_tip(int index)
{
    switch (index) {
    case 0: return _L("The most used settings.");
    case 1: return _L("Every setting.");
    default:
        return _L("The settings Bambu Studio shows, in EdgeSlicer's layout. Settings you have changed stay visible.");
    }
}

} // namespace

SettingsViewSelector::SettingsViewSelector(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    SetName(_L("Settings view"));
    SetToolTip(_L("Settings view"));

    std::vector<wxString> labels;
    for (int i = 0; i < 3; ++i)
        labels.push_back(settings_view_segment_label(i));

    m_toggle = new SegmentedToggle(this, labels, 0, SegmentedToggle::Style::Pill);
    m_toggle->setButtonMinWidth(settings_view_segment_min_width_dip);
    m_toggle->SetName(_L("Settings view"));
    for (int i = 0; i < 3; ++i)
        m_toggle->setSegmentToolTip(i, settings_view_segment_tip(i));
    m_toggle->bindSelectionCallback([this](int index) { choose(index); });

    m_combo = new ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(settings_view_combo_width_dip), -1), 0,
                           nullptr, wxCB_READONLY);
    for (int i = 0; i < 3; ++i)
        m_combo->Append(settings_view_segment_label(i));
    m_combo->SetName(_L("Settings view"));
    m_combo->SetToolTip(_L("Settings view") + "\n\n" + _L("Basic") + ": " + settings_view_segment_tip(0) + "\n" + _L("Advanced") + ": " +
                        settings_view_segment_tip(1) + "\nBambu: " + settings_view_segment_tip(2));
    m_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) { choose(m_combo->GetSelection()); });


    auto* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_toggle, 0, wxALIGN_CENTER_VERTICAL);
    sizer->Add(m_combo, 0, wxALIGN_CENTER_VERTICAL);
    SetSizer(sizer);

    // The header rows are crowded (the sidebar also holds Global / Objects and three icons): fall back to
    // the dropdown while the pill does not fit.
    parent->Bind(wxEVT_SIZE, &SettingsViewSelector::on_parent_size, this);

    update_from_app();
}

void SettingsViewSelector::choose(int index)
{
    if (index < 0 || index > 2)
        return;
    const ViewChoice choice = ViewChoice(index);
    // Re-entrancy: save_view_choice() refreshes every tab, which calls update_from_app() on this control.
    CallAfter([choice]() { wxGetApp().save_view_choice(choice); });
}

void SettingsViewSelector::update_from_app()
{
    const ViewChoice choice = wxGetApp().get_view_choice();
    m_toggle->setSelected(int(choice));
    if (m_combo->GetSelection() != int(choice))
        m_combo->SetSelection(int(choice));
    apply_visibility();
}

void SettingsViewSelector::apply_visibility()
{
    m_toggle->Show(!m_narrow);
    m_combo->Show(m_narrow);
    Layout();
    Fit();
    if (wxWindow* parent = GetParent())
        parent->Layout();
}

void SettingsViewSelector::on_parent_size(wxSizeEvent& evt)
{
    evt.Skip();
    if (m_layout_pending)
        return;
    m_layout_pending = true;
    CallAfter([this]() {
        m_layout_pending = false;
        update_layout();
    });
}

void SettingsViewSelector::update_layout()
{
    wxWindow* parent = GetParent();
    if (parent == nullptr || parent->GetSizer() == nullptr)
        return;
    const int available   = parent->GetClientSize().x;
    const int toggle_w    = m_toggle->GetBestSize().x;
    const int combo_w     = m_combo->GetBestSize().x;
    const int row_min_now = parent->GetSizer()->CalcMin().x;
    // The row's minimum width if the pill were shown instead of whatever is shown now.
    const int with_toggle = row_min_now - (m_narrow ? combo_w : toggle_w) + toggle_w;
    const bool narrow     = with_toggle > available;
    if (narrow != m_narrow) {
        m_narrow = narrow;
        apply_visibility();
    }
}

void SettingsViewSelector::msw_rescale()
{
    m_toggle->setButtonMinWidth(settings_view_segment_min_width_dip);
    m_combo->SetMinSize(wxSize(FromDIP(settings_view_combo_width_dip), -1));
    m_combo->Rescale();
    apply_visibility();
    update_layout();
}

void SettingsViewSelector::sys_color_changed()
{
    SetBackgroundColour(GetParent()->GetBackgroundColour());
    m_combo->Rescale();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
