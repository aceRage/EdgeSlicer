#ifndef slic3r_FilamentNozzleValuesDialog_hpp_
#define slic3r_FilamentNozzleValuesDialog_hpp_

// Per-nozzle process P5: the Filament tab's table of every per-nozzle value of a filament preset
// (filament_nozzle_variants). The Nozzle size switch on the Filament pages edits the nozzle-size values in place;
// this table is where values for one tool (T1..Tn, the printer's own 1-based labels) are set, and where every
// value can be seen at once. The stored text format is unchanged (libslic3r/ToolSlots.hpp, section 4b).

#include <string>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"

class wxBoxSizer;
class wxButton;
class wxChoice;
class wxScrolledWindow;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class FilamentNozzleValuesDialog : public DPIDialog
{
public:
    // `tool_labels`: one per tool of the printer, 1-based as the printer names them ("T1", "T2 (Right) 0.6 mm").
    // `sizes`: the nozzle sizes offered (those the text names are added).
    FilamentNozzleValuesDialog(wxWindow *parent, const std::string &text, const std::vector<wxString> &tool_labels,
                               std::vector<double> sizes);

    // The edited filament_nozzle_variants text.
    std::string text() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Row
    {
        wxChoice     *tool{nullptr};
        wxChoice     *size{nullptr};
        wxChoice     *flow{nullptr};
        wxChoice     *setting{nullptr};
        wxTextCtrl   *value{nullptr};
        wxStaticText *error{nullptr};
        wxButton     *remove{nullptr};
        wxBoxSizer   *sizer{nullptr};
        bool          removed{false};
    };

    void add_row(int tool, double diameter, int flow_mode, const std::string &key, const std::string &value);
    void remove_row(size_t index);
    // Shows each row's problem under it and enables OK only when there is none.
    void validate();
    // The selector and value a row holds.
    void row_values(const Row &row, int &tool, double &diameter, int &flow_mode, std::string &key, std::string &value) const;

    std::vector<std::string> m_keys;       // the settings a row can set, in the order of m_key_labels
    std::vector<wxString>    m_key_labels; // "Nozzle temperature (°C)"
    std::vector<wxString>    m_tool_labels;
    std::vector<double>      m_sizes;
    std::vector<Row>         m_rows;
    wxScrolledWindow        *m_scroll{nullptr};
    wxBoxSizer              *m_rows_sizer{nullptr};
    wxStaticText            *m_empty_note{nullptr};
    wxButton                *m_ok{nullptr};
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FilamentNozzleValuesDialog_hpp_
