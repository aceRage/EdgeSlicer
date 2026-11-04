#ifndef slic3r_GUI_ExtruderSettingsDialog_hpp_
#define slic3r_GUI_ExtruderSettingsDialog_hpp_

// The nozzle strip's data from the edited printer and the project (NozzleStripModel.hpp), how a strip edit is written back
// (the P7 / P9 project keys, one rebuild), and the Extruder Settings dialog: per extruder its nozzle size and type, per rack
// position of a rack extruder its nozzle (Bambu Studio's NozzleSettingsListDialog, AGPL-3.0, commits 464f4b8db / 785f04d79,
// as the layout reference). Nothing here changes slicing beyond the keys the sidebar already wrote.

#include "GUI_Utils.hpp"

#include "libslic3r/NozzleStripModel.hpp"

#include <vector>

#include <wx/string.h>

class ComboBox;

namespace Slic3r { namespace GUI {

// Bambu's two-extruder naming (L / R) for the edited printer.
bool                         nozzle_strip_sides();
// The edited printer with the project's sizes, flow types and racks.
NozzleStripInput             current_nozzle_strip_input();
// Its tiles; `with_state`: the current plate's filaments placed on them and the tiles no process fits marked.
std::vector<NozzleStripTile> current_nozzle_strip_tiles(const NozzleStripInput &in, bool with_state);
// The sizes a nozzle can be given (P7's list) and the flow types the printer takes.
std::vector<double>           nozzle_strip_size_choices();
std::vector<NozzleVolumeType> nozzle_strip_type_choices();
wxString                      nozzle_type_name(NozzleVolumeType type);
// "Left", "Right", "T2", "Extruder".
wxString                      nozzle_strip_extruder_name(const NozzleStripInput &in, size_t extruder);
// Writes `edit` in one batch (sizes, racks, flow types) and rebuilds once. Filaments pinned to a rack size or position the
// edit removes go back to their side's own nozzle after a confirm. False when nothing changed or the user kept editing
// (`kept_editing` says which).
bool apply_nozzle_strip_edit(wxWindow *parent, const NozzleStripInput &in, const NozzleStripEdit &edit, bool *kept_editing = nullptr);

class ExtruderSettingsDialog : public DPIDialog
{
public:
    explicit ExtruderSettingsDialog(wxWindow *parent);
    // wxID_OK: applied; wxID_REFRESH: the user asked to sync from the printer; wxID_CANCEL.

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct Row
    {
        size_t    extruder{ 0 };
        int       slot{ 0 }; // 1-based rack position, 0 = the extruder's nozzle
        ComboBox *size{ nullptr };
        ComboBox *type{ nullptr };
    };
    void on_ok();
    NozzleStripInput              m_in;
    std::vector<double>           m_sizes;
    std::vector<NozzleVolumeType> m_types;
    std::vector<Row>              m_rows;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ExtruderSettingsDialog_hpp_
