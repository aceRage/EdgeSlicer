// Per-nozzle process P6: see ProcessNozzleWidthsDialog.hpp.

#include "ProcessNozzleWidthsDialog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/NozzleProcess.hpp"
#include "libslic3r/ToolSlots.hpp"

namespace Slic3r {
namespace GUI {

namespace {

wxString width_label(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    return def == nullptr || def->label.empty() ? from_u8(key) : _(def->label);
}

std::string format_width(const FloatOrPercent &w)
{
    if (w.value == 0.)
        return "0";
    char buf[64];
    snprintf(buf, sizeof(buf), "%.4g", w.value);
    return std::string(buf) + (w.percent ? "%" : "");
}

std::string trimmed(const wxString &text)
{
    std::string s = into_u8(text);
    s.erase(0, s.find_first_not_of(" \t"));
    s.erase(s.find_last_not_of(" \t") + 1);
    return s;
}

} // namespace

std::vector<ProcessNozzleWidthsDialog::Column> ProcessNozzleWidthsDialog::columns_for(const DynamicPrintConfig &process,
                                                                                       const DynamicPrintConfig &printer)
{
    std::vector<Column> out;
    const auto         *nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter");
    if (process_nozzles_split(process) && nozzles != nullptr && nozzles->values.size() >= 2) {
        // The process keeps values per nozzle (Left / Right, T1..Tn): widths per tool too.
        int        extruder_count = 0;
        const bool sided          = nozzles->values.size() == 2 && printer.support_different_extruders(extruder_count);
        for (size_t e = 0; e < nozzles->values.size(); ++e) {
            Column c;
            c.tool     = int(e) + 1;
            c.diameter = nozzles->values[e];
            c.label    = sided ? (e == 0 ? _L("Left") : _L("Right")) : wxString::Format("T%d", int(e + 1));
            c.label += "\n" + from_u8(format_nozzle_diameter_word(c.diameter)) + " mm";
            out.push_back(c);
        }
        return out;
    }
    for (double d : printer_nozzle_sizes(printer)) {
        Column c;
        c.diameter = d;
        c.label    = from_u8(format_nozzle_diameter_word(d)) + " mm";
        out.push_back(c);
    }
    return out;
}

ProcessNozzleWidthsDialog::ProcessNozzleWidthsDialog(wxWindow *parent, const DynamicPrintConfig &process, const DynamicPrintConfig &printer)
    : DPIDialog(parent, wxID_ANY, _L("Line widths per nozzle"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_process(process)
    , m_printer(printer)
    , m_keys(nozzle_width_option_keys())
    , m_columns(columns_for(process, printer))
{
    if (const auto *opt = process.option<ConfigOptionString>("process_nozzle_line_widths"))
        m_original = opt->value;

    const int em  = em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    auto *intro = new wxStaticText(this, wxID_ANY,
                                   _L("The line widths each nozzle prints with. Leave a cell empty to use the width of the process: a "
                                      "percentage of the nozzle diameter as it is, a width in mm scaled from the nozzle the process was "
                                      "made for to this nozzle's size. The greyed value in an empty cell is the width the nozzle gets.") +
                                       "\n" +
                                       _L("A width typed in a cell is used for that nozzle only (mm, or % of the nozzle diameter). A nozzle "
                                          "that takes its speeds from a process of its own (Speeds from) uses that process's widths."));
    intro->Wrap(em * 70);
    top->Add(intro, 0, wxALL, em);

    // The nozzle the process's mm widths were made for.
    auto *ref_row = new wxBoxSizer(wxHORIZONTAL);
    ref_row->Add(new wxStaticText(this, wxID_ANY, _L("Widths made for nozzle") + ":"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    double reference = 0.;
    if (const auto *opt = process.option<ConfigOptionFloat>("line_width_nozzle_diameter"))
        reference = opt->value;
    m_reference = new wxTextCtrl(this, wxID_ANY, reference > 0. ? from_u8(format_nozzle_diameter_word(reference)) : wxString(),
                                 wxDefaultPosition, wxSize(em * 7, -1));
    m_reference->SetToolTip(_L("The nozzle diameter (mm) the process's line widths in mm were made for. Empty: the printer "
                               "preset's nozzle size."));
    ref_row->Add(m_reference, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    ref_row->Add(new wxStaticText(this, wxID_ANY, "mm"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);
    m_reference_note = new wxStaticText(this, wxID_ANY, wxEmptyString);
    ref_row->Add(m_reference_note, 1, wxALIGN_CENTER_VERTICAL);
    top->Add(ref_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    if (m_columns.empty()) {
        top->Add(new wxStaticText(this, wxID_ANY, _L("The printer has one nozzle size: the widths above apply to every nozzle.")), 0,
                 wxLEFT | wxRIGHT | wxBOTTOM, em);
    } else {
        auto *grid = new wxFlexGridSizer(int(m_columns.size()) + 1, em / 2, em);
        grid->Add(new wxStaticText(this, wxID_ANY, _L("Setting")), 0, wxALIGN_BOTTOM);
        for (const Column &c : m_columns)
            grid->Add(new wxStaticText(this, wxID_ANY, c.label), 0, wxALIGN_BOTTOM);
        for (const std::string &key : m_keys) {
            grid->Add(new wxStaticText(this, wxID_ANY, width_label(key)), 0, wxALIGN_CENTER_VERTICAL);
            std::vector<wxTextCtrl *> row;
            for (const Column &c : m_columns) {
                std::string value;
                find_filament_nozzle_value(m_original, c.tool, c.tool > 0 ? 0. : c.diameter, -1, key, &value);
                auto *cell = new wxTextCtrl(this, wxID_ANY, from_u8(value), wxDefaultPosition, wxSize(em * 8, -1));
                cell->Bind(wxEVT_TEXT, [this](wxCommandEvent &evt) {
                    evt.Skip();
                    refresh();
                });
                grid->Add(cell, 0, wxALIGN_CENTER_VERTICAL);
                row.push_back(cell);
            }
            m_cells.push_back(row);
        }
        top->Add(grid, 0, wxLEFT | wxRIGHT, em);
    }

    m_error = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_error->SetForegroundColour(wxColour("#E14747"));
    top->Add(m_error, 0, wxALL, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    m_ok         = new wxButton(this, wxID_OK, _L("OK"));
    auto *cancel = new wxButton(this, wxID_CANCEL, _L("Cancel"));
    buttons->Add(m_ok, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em / 2);
    buttons->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);

    m_reference->Bind(wxEVT_TEXT, [this](wxCommandEvent &evt) {
        evt.Skip();
        refresh();
    });

    SetSizer(top);
    refresh();
    Fit();
    SetMinSize(GetSize());
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

double ProcessNozzleWidthsDialog::reference_diameter() const
{
    const std::string s = m_reference != nullptr ? trimmed(m_reference->GetValue()) : std::string();
    if (s.empty())
        return 0.;
    try {
        size_t       used = 0;
        const double d    = std::stod(s, &used);
        return used == s.size() && std::isfinite(d) && d > 0. && d < 3. ? d : -1.;
    } catch (...) {
        return -1.;
    }
}

std::string ProcessNozzleWidthsDialog::text() const
{
    std::string out = m_original;
    for (size_t r = 0; r < m_cells.size(); ++r)
        for (size_t c = 0; c < m_cells[r].size(); ++c) {
            const Column &col = m_columns[c];
            out = set_process_nozzle_width(out, col.tool, col.tool > 0 ? 0. : col.diameter, m_keys[r], trimmed(m_cells[r][c]->GetValue()));
        }
    return out;
}

DynamicPrintConfig ProcessNozzleWidthsDialog::working_config() const
{
    DynamicPrintConfig config = m_printer;
    config.apply(m_process, true);
    config.set_key_value("process_nozzle_line_widths", new ConfigOptionString(text()));
    config.set_key_value("line_width_nozzle_diameter", new ConfigOptionFloat(std::max(0., reference_diameter())));
    return config;
}

void ProcessNozzleWidthsDialog::refresh()
{
    wxString error;
    const double reference = reference_diameter();
    if (reference < 0.)
        error = _L("The nozzle the widths were made for must be a diameter in mm, e.g. 0.4.");
    for (size_t r = 0; r < m_cells.size(); ++r)
        for (size_t c = 0; c < m_cells[r].size(); ++c) {
            const std::string value = trimmed(m_cells[r][c]->GetValue());
            if (value.empty())
                continue;
            if (const std::string problem = validate_nozzle_width_value(value); !problem.empty() && error.empty())
                error = width_label(m_keys[r]) + ", " + m_columns[c].label.BeforeFirst('\n') + ": " + from_u8(problem);
        }
    const DynamicPrintConfig config = working_config();
    if (m_reference_note != nullptr) {
        const double used = line_width_reference_diameter(config);
        m_reference_note->SetLabel(used > 0. ? format_wxstr(_L("Widths in mm are scaled from %1% mm."), format_nozzle_diameter_word(used))
                                             : wxString());
    }
    for (size_t r = 0; r < m_cells.size(); ++r)
        for (size_t c = 0; c < m_cells[r].size(); ++c) {
            const Column     &col = m_columns[c];
            const NozzleWidth w   = nozzle_width_for(config, m_keys[r], col.tool, col.diameter);
            wxString          hint = from_u8(format_width(w.value));
            if (w.source == NozzleWidthSource::Scaled)
                hint += " " + _L("(scaled)");
            else if (w.value.value == 0.)
                hint = _L("auto");
            m_cells[r][c]->SetHint(hint);
            m_cells[r][c]->SetToolTip(w.source == NozzleWidthSource::Override ? _L("Set for this nozzle.") :
                                      w.source == NozzleWidthSource::Scaled   ? _L("The process's width in mm, scaled to this nozzle's size.") :
                                                                                _L("The process's width, as it is."));
        }
    m_error->SetLabel(error);
    m_error->Show(!error.empty());
    if (m_ok != nullptr)
        m_ok->Enable(error.empty());
    Layout();
}

void ProcessNozzleWidthsDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Fit();
    Refresh();
}

} // namespace GUI
} // namespace Slic3r
