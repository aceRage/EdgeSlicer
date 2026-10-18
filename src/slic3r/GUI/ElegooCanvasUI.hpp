#ifndef slic3r_GUI_ElegooCanvasUI_hpp_
#define slic3r_GUI_ElegooCanvasUI_hpp_

// The GUI side of reading an Elegoo CANVAS (ElegooCanvas.hpp): which address to ask, asking it off
// the GUI thread behind a progress dialog, the sidebar sync's filament list, the user-facing
// failure sentences and the "Capture Canvas data" diagnostic. Design:
// tests/design_elegoo_canvas_sync_device_page.md.

#include <functional>
#include <string>
#include <vector>

#include <wx/string.h>

#include "slic3r/Utils/ElegooCanvas.hpp"
#include "filamentsync/FilamentData.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

// The selected printer preset reads CANVAS: a Centauri Carbon 1 reached through Elegoo Link (the
// preset's own host_type, or a device of its model whose host type is Elegoo Link).
bool current_printer_reads_canvas();

// Where to ask: the model's last used Elegoo Link device, else the preset's own print_host, else
// the first Elegoo Link device of the model. "" when there is none.
std::string canvas_address_for_current_printer();

// Runs `job` on a worker thread while a pulsing, app-modal progress dialog is up, and returns when
// it has finished. The job must not touch wx; it is bounded by the query's own timeouts.
void run_canvas_job(wxWindow* parent, const wxString& message, std::function<void()> job);

// canvas_query() behind run_canvas_job().
CanvasQueryResult query_canvas_with_progress(wxWindow* parent, const std::string& address);

// The trays as the sync dialog's machine list: one entry per tray in slot order, empty trays kept
// (type "", which the dialog skips) so the slot numbers line up with the printer.
std::vector<FilamentData> canvas_machine_filaments(const CanvasInfo& info);

// Why nothing was read, as one translated sentence.
wxString canvas_failure_text(const CanvasQueryResult& result);

// "Capture Canvas data": Cmd 0 / 1 / 324 with every frame, redacted, written as JSON to the log
// folder, then says where. Asks nothing of the printer beyond those three reads.
void capture_canvas_data(wxWindow* parent, const std::string& address, const std::string& printer_model);

} // namespace GUI
} // namespace Slic3r

#endif
