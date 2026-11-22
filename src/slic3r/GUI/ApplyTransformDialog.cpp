#include "ApplyTransformDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

namespace Slic3r { namespace GUI {

ApplyTransformDialog::ApplyTransformDialog(wxWindow *parent, bool position_available, bool position, bool rotation, bool scale)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("Apply Transform"), wxDefaultPosition,
                wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(3, 2, FromDIP(4), FromDIP(20));
    f_sizer->AddGrowableCol(0, 1);

    // ::Label rather than a bare wxStaticText, as in CloneDialog: it takes the dialog's background in dark mode.
    auto add_row = [this, f_sizer](const wxString &text, const wxString &tooltip, bool checked, bool enabled) {
        auto label = new ::Label(this, Label::Body_14, text + ":");
        label->SetToolTip(tooltip);
        auto checkbox = new ::CheckBox(this);
        checkbox->SetValue(checked && enabled);
        checkbox->SetToolTip(tooltip);
        if (!enabled) {
            checkbox->Disable();
            label->Disable();
        }
        checkbox->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
            e.Skip();
            update_apply_button();
        });
        f_sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL);
        f_sizer->Add(checkbox, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));
        return checkbox;
    };

    m_rotation_cb = add_row(_L("Rotation"), _L("Make the current rotation the new zero rotation"), rotation, true);
    m_scale_cb    = add_row(_L("Scale"), _L("Make the current size the new 100% scale"), scale, true);
    m_position_cb = add_row(_L("Position"),
                            position_available ? _L("Make the part's current place inside its object its new zero position") :
                                                 _L("Only for parts: an object's position is its place on the plate"),
                            position, position_available);

    v_sizer->Add(f_sizer, 1, wxEXPAND | wxALL, FromDIP(10));

    auto dlg_btns = new DialogButtons(this, {"Apply", "Cancel"});
    m_apply_btn   = dlg_btns->GetAPPLY();
    m_apply_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_OK); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    SetSizer(v_sizer);
    Layout();
    v_sizer->Fit(this);
    // Not narrower than the title needs.
    SetMinSize(wxSize(FromDIP(300), -1));
    Fit();
    update_apply_button();
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();

    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &e) {
        const int key = e.GetKeyCode();
        if (key == WXK_ESCAPE)
            EndModal(wxID_CANCEL);
        else if ((key == WXK_RETURN || key == WXK_NUMPAD_ENTER) && m_apply_btn->IsEnabled())
            EndModal(wxID_OK);
        else
            e.Skip();
    });
}

void ApplyTransformDialog::update_apply_button()
{
    m_apply_btn->Enable(rotation() || scale() || position());
}

}} // namespace Slic3r::GUI
