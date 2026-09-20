#ifndef slic3r_GUI_SliceBakeDialog_hpp_
#define slic3r_GUI_SliceBakeDialog_hpp_

// The options dialog shown before "Bake slice to mesh..." (ObjectList::bake_slice_to_mesh).
//
// Built the same way RemeshDialog is, and for the same reason: it collects options and nothing
// else - every bit of geometry lives in libslic3r/SliceBake, and the run itself is a background
// job (Jobs/SliceBakeJob).
//
// Spec: docs/superpowers/specs/2026-09-12-slice-bake-research.md (phase 1, section 4).

#include "GUI_Utils.hpp"

#include <functional>

#include "libslic3r/SliceBake.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// What to do with the mesh once it exists.
enum class SliceBakeResultMode {
    Replace = 0,  // the object becomes the bake, in place
    AddNew,       // the bake arrives as a second object beside the original
    ExportSTL,    // nothing in the scene changes; the mesh goes to a file
};

struct SliceBakeSettings
{
    SliceBakeResultMode result = SliceBakeResultMode::Replace;
    SliceBakeOptions    options;   // layer subset (all in phase 1), resolution, smoothing, gaps
};

class SliceBakeDialog : public DPIDialog
{
public:
    // The triangle count depends on the options - the contour source, the resolution and the
    // smoothing toggle all move it - so the dialog cannot be handed one number up front. It is
    // handed a COUNTER instead and re-runs it whenever a control changes, which is what makes the
    // size line live. The counter is cheap (it walks the layers and counts points; it builds no
    // geometry) so calling it on every keystroke is fine.
    using TriangleCounter = std::function<size_t(const SliceBakeOptions &)>;

    // `default_resolution` is the print's own `resolution`, via slice_bake_default_resolution().
    SliceBakeDialog(wxWindow              *parent,
                    const wxString        &object_name,
                    size_t                 layers,
                    double                 default_resolution,
                    const TriangleCounter &counter);

    // Valid after ShowModal() returns wxID_OK; also written back to AppConfig then.
    const SliceBakeSettings &settings() const { return m_settings; }

    static SliceBakeSettings load_from_config(double default_resolution);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double            read_mm(TextInput *input, double fallback) const;
    SliceBakeSettings current_settings() const;
    void              update_preview();
    void              save_to_config() const;

    SliceBakeSettings m_settings;
    wxString          m_object_name;
    size_t            m_layers      = 0;
    double            m_default_res = SLICE_BAKE_RESOLUTION_DEFAULT;
    TriangleCounter   m_counter;

    ::ComboBox *m_result_choice = nullptr;
    ::ComboBox *m_source_choice = nullptr;
    TextInput  *m_res_input     = nullptr;
    ::CheckBox *m_smooth_cb     = nullptr;
    ::CheckBox *m_close_cb      = nullptr;
    TextInput  *m_close_input   = nullptr;
    ::Label    *m_size_text     = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SliceBakeDialog_hpp_
