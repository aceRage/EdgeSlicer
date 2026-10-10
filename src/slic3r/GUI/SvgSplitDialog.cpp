#include "SvgSplitDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "format.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <wx/bitmap.h>
#include <wx/bmpcbox.h>
#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

namespace {

wxBitmap svgsplit_swatch(wxWindow *window, const std::array<uint8_t, 3> &c, bool none = false)
{
    wxImage image(window->FromDIP(22), window->FromDIP(14));
    image.SetRGB(wxRect(0, 0, image.GetWidth(), image.GetHeight()), c[0], c[1], c[2]);
    if (none) {
        // Default filament: grey with a diagonal line
        for (int x = 0; x < image.GetWidth(); ++x) {
            int y = image.GetHeight() - 1 - x * image.GetHeight() / std::max(1, image.GetWidth());
            if (y >= 0 && y < image.GetHeight())
                image.SetRGB(x, y, 90, 90, 90);
        }
    }
    return wxBitmap(image);
}

// Physical filaments of the project ("#rrggbb" each), the same list the Image trace dialog offers
std::vector<std::string> svgsplit_project_filaments()
{
    Plater *plater = wxGetApp().plater();
    return plater != nullptr ? plater->get_extruder_colors_from_plater_config(nullptr, false) : std::vector<std::string>{};
}

// Free slot for one more physical filament (the same limits as the sidebar's "+" button)
bool svgsplit_can_add_filament()
{
    PresetBundle *pb = wxGetApp().preset_bundle;
    if (pb == nullptr || wxGetApp().plater() == nullptr || wxGetApp().plater()->printer_technology() != ptFFF)
        return false;
    const size_t physical = pb->num_physical_filaments();
    return physical < MAXIMUM_EXTRUDER_NUMBER && pb->mixed_filaments.total_filaments(physical) < MAXIMUM_FILAMENT_NUMBER;
}

class SvgSplitFilamentDialog : public wxDialog
{
public:
    SvgSplitFilamentDialog(wxWindow *parent, const wxString &file_name, const std::vector<SvgSplitColor> &colors, size_t part_count)
        : wxDialog(parent, wxID_ANY, _L("Filaments of SVG colours"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
        , m_colors(colors)
    {
        m_filaments = svgsplit_project_filaments();
        m_user.assign(colors.size(), false);

        wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);
        wxString    text = GUI::format_wxstr(_L("\"%1%\" is split into %2% parts of %3% colours. Choose a filament for each colour."),
                                             file_name, part_count, colors.size());
        auto *intro = new wxStaticText(this, wxID_ANY, text);
        intro->Wrap(FromDIP(460));
        root->Add(intro, 0, wxALL, FromDIP(12));

        auto *panel = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
        panel->SetScrollRate(0, FromDIP(10));
        auto *grid = new wxFlexGridSizer(4, FromDIP(6), FromDIP(10));
        grid->AddGrowableCol(3);
        for (size_t i = 0; i < colors.size(); ++i) {
            const SvgSplitColor &c = colors[i];
            grid->Add(new wxStaticBitmap(panel, wxID_ANY, svgsplit_swatch(this, c.color)), 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(new wxStaticText(panel, wxID_ANY, from_u8(svg_split_color_to_hex(c.color))), 0, wxALIGN_CENTER_VERTICAL);
            wxString info = c.parts.size() == 1 ? _L("1 part") : GUI::format_wxstr(_L("%1% parts"), c.parts.size());
            info += ", " + wxString::Format(L"%.1f mm\u00B2", c.area);
            auto *info_text = new wxStaticText(panel, wxID_ANY, info);
            grid->Add(info_text, 0, wxALIGN_CENTER_VERTICAL);
            auto *combo = new wxBitmapComboBox(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(170), -1), 0, nullptr,
                                               wxCB_READONLY);
            combo->SetToolTip(_L("Filament of the parts of this colour. By default the filament with the nearest colour."));
            combo->Bind(wxEVT_COMBOBOX, [this, i](wxCommandEvent &) {
                m_user[i] = true;
                update_unmatched();
            });
            grid->Add(combo, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
            m_combos.push_back(combo);
            m_infos.push_back(info_text);
        }
        panel->SetSizer(grid);
        grid->FitInside(panel);
        int rows_height = grid->GetMinSize().GetHeight();
        panel->SetMinSize(wxSize(grid->GetMinSize().GetWidth() + FromDIP(20), std::min(rows_height, FromDIP(360))));
        root->Add(panel, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(12));

        m_add = new wxButton(this, wxID_ANY, _L("Add filaments for unmatched colours"));
        m_add->SetToolTip(_L("Add a new filament of the colour for every colour without a close filament in the project."));
        m_add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { add_unmatched_filaments(); });
        root->Add(m_add, 0, wxALL, FromDIP(12));

        wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
        buttons->AddButton(new wxButton(this, wxID_OK));
        auto *cancel = new wxButton(this, wxID_CANCEL);
        cancel->SetToolTip(_L("Keep all parts on the default filament of the object"));
        buttons->AddButton(cancel);
        buttons->Realize();
        root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

        fill_combos({});
        SetSizerAndFit(root);
        CenterOnParent();
        wxGetApp().UpdateDlgDarkUI(this);
    }

    // per colour, 1 based filament or 0
    std::vector<int> extruders() const
    {
        std::vector<int> result;
        for (const wxBitmapComboBox *combo : m_combos)
            result.push_back(std::max(0, combo->GetSelection()));
        return result;
    }

private:
    bool is_unmatched(size_t i) const
    {
        double d = 0.;
        svg_split_nearest_filament(m_colors[i].color, m_filaments, &d);
        return d > SVG_SPLIT_UNMATCHED_DELTA_E;
    }

    // keep: selection per colour to keep (user choices), others get the nearest filament
    void fill_combos(const std::vector<int> &keep)
    {
        for (size_t i = 0; i < m_combos.size(); ++i) {
            wxBitmapComboBox *combo = m_combos[i];
            combo->Clear();
            combo->Append(_L("Default"), svgsplit_swatch(this, {200, 200, 200}, true));
            for (size_t f = 0; f < m_filaments.size(); ++f) {
                std::array<uint8_t, 3> rgb{128, 128, 128};
                svg_split_parse_hex(m_filaments[f], rgb);
                combo->Append(wxString::Format(_L("Filament %d"), int(f + 1)) + " " + from_u8(m_filaments[f]), svgsplit_swatch(this, rgb));
            }
            int selection = (i < keep.size() && m_user[i]) ? keep[i] : svg_split_nearest_filament(m_colors[i].color, m_filaments);
            combo->SetSelection(std::clamp(selection, 0, int(m_filaments.size())));
        }
        update_unmatched();
    }

    void update_unmatched()
    {
        size_t unmatched = 0;
        for (size_t i = 0; i < m_combos.size(); ++i) {
            bool distant = is_unmatched(i);
            m_infos[i]->SetForegroundColour(distant ? wxColour("#D9534F") : wxNullColour);
            m_infos[i]->SetToolTip(distant ? _L("No filament of a close colour in the project") : wxString());
            if (distant && !m_user[i])
                ++unmatched;
        }
        m_add->Enable(unmatched > 0 && svgsplit_can_add_filament());
        Refresh();
    }

    void add_unmatched_filaments()
    {
        std::vector<int> keep     = extruders();
        const size_t     physical = m_filaments.size();
        size_t           added    = 0;
        std::vector<std::pair<size_t, int>> new_selection;
        for (size_t i = 0; i < m_colors.size(); ++i) {
            if (m_user[i] || !is_unmatched(i) || !svgsplit_can_add_filament())
                continue;
            // an earlier added filament can already be close to this colour
            double d = 0.;
            std::vector<std::string> with_added = svgsplit_project_filaments();
            int nearest = svg_split_nearest_filament(m_colors[i].color, with_added, &d);
            if (d <= SVG_SPLIT_UNMATCHED_DELTA_E) {
                new_selection.push_back({i, nearest});
                continue;
            }
            const size_t before = wxGetApp().preset_bundle->num_physical_filaments();
            const std::string hex = svg_split_color_to_hex(m_colors[i].color);
            wxGetApp().sidebar().add_custom_filament(wxColour(from_u8(hex)));
            const size_t after = wxGetApp().preset_bundle->num_physical_filaments();
            if (after <= before)
                break; // refused (no free slot)
            ++added;
            new_selection.push_back({i, int(after)});
        }
        m_filaments = svgsplit_project_filaments();
        // Filaments are appended after the physical ones, user choices keep their filament
        for (size_t i = 0; i < keep.size(); ++i)
            if (m_user[i] && keep[i] > int(physical))
                keep[i] += int(added);
        fill_combos(keep);
        for (const auto &[i, f] : new_selection)
            m_combos[i]->SetSelection(std::clamp(f, 0, int(m_filaments.size())));
        update_unmatched();
    }

    std::vector<SvgSplitColor>      m_colors;
    std::vector<std::string>        m_filaments;
    std::vector<bool>               m_user;
    std::vector<wxBitmapComboBox *> m_combos;
    std::vector<wxStaticText *>     m_infos;
    wxButton                       *m_add = nullptr;
};

} // namespace

SvgDropAction ask_svg_drop_action(wxWindow *parent, const wxString &file_name)
{
    wxDialog dialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), wxID_ANY, _L("SVG dropped"), wxDefaultPosition,
                    wxDefaultSize, wxDEFAULT_DIALOG_STYLE);
    wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);
    root->Add(new wxStaticText(&dialog, wxID_ANY, GUI::format_wxstr(_L("How should \"%1%\" be imported?"), file_name)), 0, wxALL,
              dialog.FromDIP(12));

    struct Choice
    {
        SvgDropAction action;
        wxString      label;
        wxString      tooltip;
    };
    const Choice choices[] = {
        {SvgDropAction::Plain, _L("SVG"), _L("One part: all shapes of the SVG together, in one filament.")},
        {SvgDropAction::Split, _L("SVG (Split)"),
         _L("One part per shape. Shapes painted later cut away what they cover, so the parts keep the picture; "
            "the colours of the SVG can get their own filaments.")},
    };
    SvgDropAction result = SvgDropAction::Cancel;
    for (const Choice &c : choices) {
        wxButton *button = new wxButton(&dialog, wxID_ANY, c.label);
        button->SetToolTip(c.tooltip);
        button->Bind(wxEVT_BUTTON, [&dialog, &result, action = c.action](wxCommandEvent &) {
            result = action;
            dialog.EndModal(wxID_OK);
        });
        root->Add(button, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    }
    wxStdDialogButtonSizer *buttons = new wxStdDialogButtonSizer();
    buttons->AddButton(new wxButton(&dialog, wxID_CANCEL));
    buttons->Realize();
    root->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dialog.FromDIP(12));
    dialog.SetSizerAndFit(root);
    dialog.CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(&dialog);
    if (dialog.ShowModal() != wxID_OK)
        return SvgDropAction::Cancel;
    return result;
}

bool ask_svg_split_filaments(wxWindow *parent, const wxString &file_name, const std::vector<SvgSplitColor> &colors, size_t part_count,
                             std::vector<int> &extruders)
{
    extruders.assign(colors.size(), 0);
    if (colors.empty())
        return false;
    SvgSplitFilamentDialog dialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe), file_name, colors, part_count);
    if (dialog.ShowModal() != wxID_OK)
        return false;
    extruders = dialog.extruders();
    return true;
}

}} // namespace Slic3r::GUI
