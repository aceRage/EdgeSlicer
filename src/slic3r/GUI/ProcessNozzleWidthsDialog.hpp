#ifndef slic3r_ProcessNozzleWidthsDialog_hpp_
#define slic3r_ProcessNozzleWidthsDialog_hpp_

// Per-nozzle process P6: the Process tab's "Line widths per nozzle" table. One row per line width setting, one
// column per nozzle size of the printer (or per tool when the process is edited per nozzle). A cell holds the width
// set for that nozzle (process_nozzle_line_widths); an empty cell shows, greyed, the width the nozzle prints with:
// the process's own width as it is, or scaled to the nozzle's size (libslic3r/NozzleProcess.hpp, P6 rules).

#include <string>
#include <vector>

#include <wx/string.h>

#include "GUI_Utils.hpp"
#include "libslic3r/PrintConfig.hpp"

class wxButton;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class ProcessNozzleWidthsDialog : public DPIDialog
{
public:
    struct Column
    {
        int      tool{0};       // 1-based, 0 = a nozzle size column
        double   diameter{0.};  // the nozzle's size
        wxString label;
    };

    // `process`: the edited process preset; `printer`: the edited printer preset.
    ProcessNozzleWidthsDialog(wxWindow *parent, const DynamicPrintConfig &process, const DynamicPrintConfig &printer);

    // The edited process_nozzle_line_widths text and line_width_nozzle_diameter.
    std::string text() const;
    double      reference_diameter() const;

    // The columns the table shows for a process / printer: one per nozzle size, or one per tool when the process keeps
    // values per nozzle (process_extruder_values). Testable outside the dialog.
    static std::vector<Column> columns_for(const DynamicPrintConfig &process, const DynamicPrintConfig &printer);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    // Re-reads every cell, shows the effective widths of the empty ones and the problems; OK only without problems.
    void refresh();
    DynamicPrintConfig working_config() const;

    DynamicPrintConfig                     m_process;
    DynamicPrintConfig                     m_printer;
    std::vector<std::string>               m_keys;
    std::vector<Column>                    m_columns;
    std::vector<std::vector<wxTextCtrl *>> m_cells; // [row][column]
    wxTextCtrl                            *m_reference{nullptr};
    wxStaticText                          *m_reference_note{nullptr};
    wxStaticText                          *m_error{nullptr};
    wxButton                              *m_ok{nullptr};
    std::string                            m_original; // the text as it came: entries without a cell are kept
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_ProcessNozzleWidthsDialog_hpp_
