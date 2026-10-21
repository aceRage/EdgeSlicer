#ifndef slic3r_GUI_ThirdPartyFilamentMapPanel_hpp_
#define slic3r_GUI_ThirdPartyFilamentMapPanel_hpp_

#include "ThirdPartyFilamentMap.hpp"

#include <functional>
#include <vector>
#include <wx/panel.h>
#include <wx/string.h>

class wxBoxSizer;
class wxWrapSizer;

namespace Slic3r {
namespace GUI {

class MaterialItem;
class ThirdPartySlotPopup;

// Bambu-style mapping rows for a third-party send dialog: MaterialItem colour cards (slicer
// filament on top, device slot underneath) and a MappingItem tile popup grouped by unit
// (1A–1D). The Bambu SelectMachineDialog is not used and is not changed.
//
//   ThirdPartyFilamentMapPanel* panel = new ThirdPartyFilamentMapPanel(parent);
//   panel->set_model(model, [this]() { update_mapping_note(); });
//   std::string sent = panel->filament_mapping(); // "0:1,1:2"
//
// Read-only mode (Creality K3 holders): set_model() with model.read_only, or set_read_only(true)
// — no popup, no dropdown arrow, just the two colours.
class ThirdPartyFilamentMapPanel : public wxPanel
{
public:
    using ChangedFn = std::function<void()>;

    explicit ThirdPartyFilamentMapPanel(wxWindow* parent);
    ~ThirdPartyFilamentMapPanel() override;

    // Replaces the rows. `on_changed` is called whenever a slot is picked (not in read-only).
    void set_model(ThirdPartyFilamentMapModel model, ChangedFn on_changed = {});
    const ThirdPartyFilamentMapModel& model() const { return m_model; }

    void set_read_only(bool read_only);
    bool is_read_only() const { return m_model.read_only; }

    // Grey out the cards and skip the popup (QIDI Box off / Use CFS off).
    void set_mapping_enabled(bool enabled);
    bool mapping_enabled() const { return m_mapping_enabled; }

    // Positions in model.slots, -1 = "As sliced". Same shape as PrintHostSendDialog::mapping_choice_positions().
    // In read-only this is the visual auto-match only. The map that is sent is filament_mapping()
    // (empty); mismatch_warning() compares each filament with its T-number slot (identity).
    std::vector<int> choice_positions() const { return m_model.mapping; }
    // "0:1,1:2" — filament index to Slot::index. Empty when read_only or every row is "As sliced".
    // Read-only consumers must take the sent map from here, not from choice_positions().
    std::string filament_mapping() const;

    // Type / colour mismatch sentences for the dialog's mapping note; "" = none. Translated.
    // Read-only: identity (T-number slot), not the visual auto-match in choice_positions().
    wxString mismatch_warning() const;

    void msw_rescale();
    void sys_color_changed();

private:
    friend class ThirdPartySlotPopup;

    void rebuild_rows();
    void apply_slot_to_item(size_t row);
    void open_popup(size_t row);
    void pick_slot(size_t row, int slot_pos); // -1 = as sliced
    void restore_after_popup();
    wxColour colour_of(const std::string& raw) const;
    wxString material_label(const ThirdPartySlicerFilament& f) const;

    ThirdPartyFilamentMapModel m_model;
    ChangedFn                  m_on_changed;
    bool                       m_mapping_enabled { true };
    wxWrapSizer*               m_wrap { nullptr };
    std::vector<MaterialItem*> m_items;
    ThirdPartySlotPopup*       m_popup { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif
