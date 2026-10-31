// Per-nozzle process P5: see FilamentNozzleValuesDialog.hpp.

#include "FilamentNozzleValuesDialog.hpp"

#include <algorithm>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ToolSlots.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxString setting_label(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr)
        return from_u8(key);
    wxString label = def->label.empty() ? from_u8(key) : _(def->label);
    if (!def->sidetext.empty())
        label += " (" + _(def->sidetext) + ")";
    return label;
}

wxString size_label(double diameter) { return from_u8(format_nozzle_diameter_word(diameter)) + " mm"; }

} // namespace

FilamentNozzleValuesDialog::FilamentNozzleValuesDialog(wxWindow *parent, const std::string &text, const std::vector<wxString> &tool_labels,
                                                       std::vector<double> sizes)
    : DPIDialog(parent, wxID_ANY, _L("Values per nozzle"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_tool_labels(tool_labels)
{
    // The settings: every per-nozzle filament value the format can hold, by their labels.
    for (const std::string &key : Preset::filament_options())
        if (is_filament_nozzle_size_key(key))
            m_keys.push_back(key);
    std::vector<FilamentNozzleVariant> variants = parse_filament_nozzle_variants(text);
    for (const FilamentNozzleVariant &v : variants) {
        for (const auto &kv : v.values)
            if (std::find(m_keys.begin(), m_keys.end(), kv.first) == m_keys.end())
                m_keys.push_back(kv.first); // kept, so a row is never silently changed; validation names the problem
        if (v.diameter > 0. && std::none_of(sizes.begin(), sizes.end(), [&v](double d) { return same_nozzle_diameter(d, v.diameter); }))
            sizes.push_back(v.diameter);
        if (v.tool > int(m_tool_labels.size()))
            for (int t = int(m_tool_labels.size()) + 1; t <= v.tool; ++t)
                m_tool_labels.push_back(wxString::Format("T%d", t) + " " + _L("(not on this printer)"));
    }
    std::sort(sizes.begin(), sizes.end());
    m_sizes = std::move(sizes);
    std::vector<std::pair<wxString, std::string>> by_label;
    for (const std::string &key : m_keys)
        by_label.emplace_back(setting_label(key), key);
    std::sort(by_label.begin(), by_label.end(), [](const auto &a, const auto &b) { return a.first.CmpNoCase(b.first) < 0; });
    m_keys.clear();
    for (const auto &[label, key] : by_label) {
        m_key_labels.push_back(label);
        m_keys.push_back(key);
    }

    const int em    = em_unit();
    auto     *top   = new wxBoxSizer(wxVERTICAL);
    auto     *intro = new wxStaticText(this, wxID_ANY,
                                       _L("Values this filament takes on particular nozzles. A row applies when the filament prints on a "
                                          "nozzle that matches all of its choices; the more specific row wins (tool, then nozzle size, "
                                          "then flow type).") + "\n" +
                                           _L("Tools are numbered as on the printer, T1 to Tn. The G-code counts the same tools from 0, "
                                              "so T1 here is T0 in the G-code.") + "\n" +
                                           _L("Values for a nozzle size are easier to edit on the Filament pages: choose the size with "
                                              "the Nozzle size switch above the settings."));
    intro->Wrap(em * 70);
    top->Add(intro, 0, wxALL, em);

    // Problems in the stored text that no row can show: those parts are dropped when the table is saved.
    std::vector<std::string> parse_errors;
    parse_filament_nozzle_variants(text, &parse_errors);
    if (!parse_errors.empty()) {
        wxString msg = _L("These parts of the stored values could not be read and will be dropped when you press OK:");
        for (const std::string &e : parse_errors)
            msg += "\n  " + from_u8(e);
        auto *warn = new wxStaticText(this, wxID_ANY, msg);
        warn->SetForegroundColour(wxColour("#E14747"));
        top->Add(warn, 0, wxLEFT | wxRIGHT | wxBOTTOM, em);
    }

    auto *header = new wxFlexGridSizer(1, 5, 0, em);
    for (const wxString &title : {_L("Tool"), _L("Nozzle size"), _L("Flow type"), _L("Setting"), _L("Value")})
        header->Add(new wxStaticText(this, wxID_ANY, title), 0, wxALIGN_CENTER_VERTICAL);
    top->Add(header, 0, wxLEFT | wxRIGHT, em);

    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(em * 72, em * 22), wxVSCROLL);
    m_scroll->SetScrollRate(0, em);
    m_rows_sizer = new wxBoxSizer(wxVERTICAL);
    m_empty_note = new wxStaticText(m_scroll, wxID_ANY, _L("No values per nozzle yet. Add a row to set one."));
    m_rows_sizer->Add(m_empty_note, 0, wxALL, em / 2);
    m_scroll->SetSizer(m_rows_sizer);
    top->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, em);

    for (const FilamentNozzleVariant &v : variants)
        for (const auto &[key, value] : v.values)
            add_row(v.tool, v.diameter, v.flow_mode, key, value);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    auto *add     = new wxButton(this, wxID_ANY, _L("Add row"));
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        add_row(0, m_sizes.empty() ? 0. : m_sizes.front(), -1, m_keys.empty() ? std::string() : m_keys.front(), std::string());
        validate();
    });
    buttons->Add(add, 0, wxALIGN_CENTER_VERTICAL);
    buttons->AddStretchSpacer();
    m_ok         = new wxButton(this, wxID_OK, _L("OK"));
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    buttons->Add(m_ok, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);

    SetSizer(top);
    validate();
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void FilamentNozzleValuesDialog::add_row(int tool, double diameter, int flow_mode, const std::string &key, const std::string &value)
{
    const int em = em_unit();
    Row       row;
    row.sizer = new wxBoxSizer(wxVERTICAL);
    auto *line = new wxBoxSizer(wxHORIZONTAL);

    wxArrayString tools;
    tools.Add(_L("Any tool"));
    for (const wxString &label : m_tool_labels)
        tools.Add(label);
    row.tool = new wxChoice(m_scroll, wxID_ANY, wxDefaultPosition, wxSize(em * 11, -1), tools);
    row.tool->SetSelection(tool > 0 && tool <= int(m_tool_labels.size()) ? tool : 0);
    row.tool->SetToolTip(_L("The tool as the printer numbers it (T1 to Tn). The G-code counts from 0: T1 here is T0 in the G-code."));

    wxArrayString sizes;
    sizes.Add(_L("Any size"));
    int size_sel = 0;
    for (size_t i = 0; i < m_sizes.size(); ++i) {
        sizes.Add(size_label(m_sizes[i]));
        if (diameter > 0. && same_nozzle_diameter(diameter, m_sizes[i]))
            size_sel = int(i) + 1;
    }
    row.size = new wxChoice(m_scroll, wxID_ANY, wxDefaultPosition, wxSize(em * 9, -1), sizes);
    row.size->SetSelection(size_sel);

    wxArrayString flows;
    flows.Add(_L("Any flow"));
    flows.Add(_L("Standard flow"));
    flows.Add(_L("High flow"));
    row.flow = new wxChoice(m_scroll, wxID_ANY, wxDefaultPosition, wxSize(em * 9, -1), flows);
    row.flow->SetSelection(flow_mode == int(fvtStandard) ? 1 : flow_mode == int(fvtHighFlow) ? 2 : 0);

    wxArrayString settings;
    for (const wxString &label : m_key_labels)
        settings.Add(label);
    row.setting = new wxChoice(m_scroll, wxID_ANY, wxDefaultPosition, wxSize(em * 22, -1), settings);
    const auto it = std::find(m_keys.begin(), m_keys.end(), key);
    row.setting->SetSelection(it == m_keys.end() ? (m_keys.empty() ? wxNOT_FOUND : 0) : int(it - m_keys.begin()));

    row.value = new wxTextCtrl(m_scroll, wxID_ANY, from_u8(value), wxDefaultPosition, wxSize(em * 9, -1));
    row.remove = new wxButton(m_scroll, wxID_ANY, _L("Remove"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);

    for (wxWindow *w : std::initializer_list<wxWindow *>{row.tool, row.size, row.flow, row.setting, row.value})
        line->Add(w, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    line->Add(row.remove, 0, wxALIGN_CENTER_VERTICAL);
    row.sizer->Add(line, 0);
    row.error = new wxStaticText(m_scroll, wxID_ANY, wxEmptyString);
    row.error->SetForegroundColour(wxColour("#E14747"));
    row.error->Hide();
    row.sizer->Add(row.error, 0, wxLEFT | wxBOTTOM, em / 2);

    const size_t index = m_rows.size();
    auto on_change = [this](wxCommandEvent &evt) {
        evt.Skip();
        validate();
    };
    row.tool->Bind(wxEVT_CHOICE, on_change);
    row.size->Bind(wxEVT_CHOICE, on_change);
    row.flow->Bind(wxEVT_CHOICE, on_change);
    row.setting->Bind(wxEVT_CHOICE, on_change);
    row.value->Bind(wxEVT_TEXT, on_change);
    row.remove->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent &) {
        // Removed after the click returns: the button is a child of the row being destroyed.
        CallAfter([this, index]() { remove_row(index); });
    });

    m_rows_sizer->Add(row.sizer, 0, wxEXPAND | wxTOP, em / 4);
    m_rows.push_back(row);
    m_scroll->FitInside();
    m_scroll->Layout();
}

void FilamentNozzleValuesDialog::remove_row(size_t index)
{
    if (index >= m_rows.size() || m_rows[index].removed)
        return;
    Row &row = m_rows[index];
    for (wxWindow *w : std::initializer_list<wxWindow *>{row.tool, row.size, row.flow, row.setting, row.value, row.remove, row.error})
        w->Destroy();
    m_rows_sizer->Detach(row.sizer);
    delete row.sizer; // its windows are gone; an empty sizer
    row = Row();
    row.removed = true;
    m_scroll->FitInside();
    m_scroll->Layout();
    validate();
}

void FilamentNozzleValuesDialog::row_values(const Row &row, int &tool, double &diameter, int &flow_mode, std::string &key, std::string &value) const
{
    tool                = std::max(0, row.tool->GetSelection());
    const int size_sel  = row.size->GetSelection();
    diameter            = size_sel > 0 && size_t(size_sel) <= m_sizes.size() ? m_sizes[size_t(size_sel) - 1] : 0.;
    const int flow_sel  = row.flow->GetSelection();
    flow_mode           = flow_sel == 1 ? int(fvtStandard) : flow_sel == 2 ? int(fvtHighFlow) : -1;
    const int key_sel   = row.setting->GetSelection();
    key                 = key_sel >= 0 && size_t(key_sel) < m_keys.size() ? m_keys[size_t(key_sel)] : std::string();
    value               = into_u8(row.value->GetValue());
    value.erase(0, value.find_first_not_of(" \t"));
    value.erase(value.find_last_not_of(" \t") + 1);
}

void FilamentNozzleValuesDialog::validate()
{
    bool                     ok = true, any = false;
    std::vector<std::string> seen; // selector + key of the rows above
    for (Row &row : m_rows) {
        if (row.removed)
            continue;
        any = true;
        int         tool, flow_mode;
        double      diameter;
        std::string key, value;
        row_values(row, tool, diameter, flow_mode, key, value);
        wxString error;
        if (tool == 0 && diameter <= 0. && flow_mode < 0)
            error = _L("Choose a tool, a nozzle size or a flow type.");
        else if (key.empty())
            error = _L("Choose a setting.");
        else if (const std::string problem = validate_filament_nozzle_value(key, value); !problem.empty())
            error = from_u8(problem);
        else {
            const std::string id = format_filament_nozzle_selector(tool, diameter, flow_mode) + "|" + key;
            if (std::find(seen.begin(), seen.end(), id) != seen.end())
                error = _L("Another row already sets this setting for the same nozzle.");
            seen.push_back(id);
        }
        row.error->SetLabel(error);
        row.error->Show(!error.empty());
        ok &= error.empty();
    }
    m_empty_note->Show(!any);
    if (m_ok != nullptr)
        m_ok->Enable(ok);
    m_scroll->FitInside();
    m_scroll->Layout();
}

std::string FilamentNozzleValuesDialog::text() const
{
    std::string out;
    for (const Row &row : m_rows) {
        if (row.removed)
            continue;
        int         tool, flow_mode;
        double      diameter;
        std::string key, value;
        row_values(row, tool, diameter, flow_mode, key, value);
        if (key.empty() || value.empty())
            continue;
        out = set_filament_nozzle_value(out, tool, diameter, flow_mode, key, value);
    }
    return out;
}

void FilamentNozzleValuesDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
