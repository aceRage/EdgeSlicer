#ifndef slic3r_GUI_HeightPatternDialog_hpp_
#define slic3r_GUI_HeightPatternDialog_hpp_

// The "Height colour pattern" dialog (tests/design_height_colour_pattern.md, phase 2).
//
// The dialog only collects a HeightPatternInput and shows the live result; applying it to the
// object is ObjectList's job (GUI_ObjectHeightPattern.cpp). The maths, the validation and the
// wording of the live text are in libslic3r/HeightPatternInput.hpp, which has the unit tests.
//
// This header deliberately includes no Widgets/*.hpp: they and Field.hpp both define a CheckBox,
// and a header that pulled one in would break the unity build for every file that includes it.

#include "GUI_Utils.hpp"

#include "libslic3r/HeightPatternInput.hpp"

#include <functional>
#include <string>
#include <vector>

class TextInput;
class CheckBox;
class ComboBox;
class Label;
class Button;

namespace Slic3r {

class ModelObject;

namespace GUI {

// What the dialog needs to know about the object it is for. All Z values are object-local mm.
struct HeightPatternDialogContext
{
    std::string                object_name;
    const ModelObject         *object        = nullptr; // for the overlap check; may be null
    int                        own_pattern_id = 0;      // > 0 when editing an existing pattern
    std::vector<double>        layer_tops;              // the object's real layer grid
    double                     layer_height  = 0.2;
    double                     object_height = 0.;
    int                        max_filament  = 1;       // physical + mixed
    std::vector<std::string>   filament_colours;        // "#RRGGBB" per filament, index 0 = filament 1
    // Other objects on the plate the pattern could also go on (0 hides the check-box).
    int                        other_plate_objects = 0;
    // True when those objects are not the same height as this one.
    bool                       other_heights_differ = false;
    // Phase 3: what the change-count line needs (the object's own filament, the project's purge table).
    HeightPatternChangeInput   change_input;
    // Phase 3: the pattern's bands the user has edited by hand (edit mode), for the note under the list.
    std::vector<HeightPatternBandDiff> hand_edits;
    // Phase 3: called with the snapped ranges on every change (empty when the input is not usable), so the
    // 3D view can draw the bands live.
    std::function<void(const std::vector<HeightPatternRange> &)> on_preview;
};

struct HeightPatternDialogResult
{
    HeightPatternInput input;
    bool               all_plate_objects = false;
    // The dialog showed the overlap with this object's existing ranges and the user chose to
    // replace them (the button said so). Other objects are asked about afterwards.
    bool               replace_confirmed = false;
};

class HeightPatternSwatches; // defined in the .cpp
class HeightPatternStrip;    // defined in the .cpp
class HeightPatternBandList; // defined in the .cpp

class HeightPatternDialog : public DPIDialog
{
public:
    HeightPatternDialog(wxWindow *parent, const HeightPatternDialogContext &ctx, const HeightPatternInput &initial);

    // Valid after ShowModal() returned wxID_OK. Also remembered in AppConfig.
    const HeightPatternDialogResult &result() const { return m_result; }

    // The defaults a new pattern starts from: the last used values, else the owner's example.
    static HeightPatternInput load_last_input();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    HeightPatternInput current_input() const;
    void               update_preview();
    void               refit();   // keeps the dialog at its fixed width
    void               update_unit_labels();
    void               update_gap_controls();
    void               rebuild_sequence();
    void               save_last_input() const;
    double             read_number(::TextInput *input, double fallback) const;

    HeightPatternDialogContext m_ctx;
    HeightPatternInput         m_input;       // sequence, gap filament and the like live here
    HeightPatternDialogResult  m_result;
    bool                       m_has_clash = false;

    ::ComboBox *m_unit_combo    = nullptr;
    ::CheckBox *m_snap_cb       = nullptr;
    ::TextInput *m_start_input   = nullptr;
    ::TextInput *m_band_input    = nullptr;
    ::TextInput *m_gap_input     = nullptr;
    ::TextInput *m_repeat_input  = nullptr;
    ::CheckBox *m_to_top_cb     = nullptr;
    ::ComboBox *m_gap_mode      = nullptr;
    ::ComboBox *m_gap_filament  = nullptr;
    ::Label *m_gap_filament_label = nullptr;
    ::CheckBox *m_all_cb        = nullptr;
    ::Label *m_result_text   = nullptr;
    ::Label *m_clash_text    = nullptr;
    ::Label *m_error_text    = nullptr;
    ::Label *m_note_text     = nullptr;
    ::Label *m_change_text   = nullptr;
    ::Label *m_edits_text    = nullptr;
    ::Button *m_ok_button     = nullptr;
    HeightPatternSwatches *m_palette  = nullptr;
    HeightPatternSwatches *m_sequence = nullptr;
    HeightPatternStrip    *m_strip    = nullptr;
    HeightPatternBandList *m_band_list = nullptr;
    ::Label               *m_help_text = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_HeightPatternDialog_hpp_
