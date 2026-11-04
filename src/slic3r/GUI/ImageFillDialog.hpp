#ifndef slic3r_GUI_ImageFillDialog_hpp_
#define slic3r_GUI_ImageFillDialog_hpp_

// "Apply image fill...": pick a picture (or a gradient), a projection, the filaments it may use
// and how finely to subdivide, and see the result before applying it.
// Spec: docs/superpowers/specs/2026-09-07-imagemap-phase2-imagefill.md

#include "GUI_Utils.hpp"

#include "libslic3r/ImageFill.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

class wxBitmapButton;
class wxCheckBox;
class wxCheckListBox;
class wxChoice;
class wxSpinCtrl;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

// One loaded filament, as the dialog needs it.
struct ImageFillFilament
{
    int                  id = 0;          // 1-based
    std::array<float, 3> color{0, 0, 0};  // sRGB 0..1
    std::string          name;            // for the checkbox label
};

class ImageFillDialog : public DPIDialog
{
public:
    // `filaments` are every loaded filament, physical and mixed, in id order. `initial` is what a
    // previous application recorded on the part (image_fill_params), so re-opening the dialog on
    // an already-filled part starts where the user left off. `mesh` and `existing` are the part
    // itself, used only for the preview - Apply re-runs the fill on the real volume.
    // `initial_row_surfaces` is which surfaces the part is ALREADY re-sampling through an enabled
    // ImageWeighted row - a bitmask of IMAGE_FILL_SURFACE_TOP / _WALLS, which
    // Plater::apply_image_fill works out from the part's own bindings before constructing the
    // dialog - so re-opening the dialog on a part using the feature starts with the same boxes
    // ticked, the same promise `initial` makes for the projection settings.
    ImageFillDialog(wxWindow                                      *parent,
                    const std::vector<ImageFillFilament>          &filaments,
                    const ImageFillParams                         &initial,
                    ImageAssetStore                               *assets,
                    const indexed_triangle_set                    &mesh,
                    const TriangleSelector::TriangleSplittingData &existing,
                    const std::vector<int>                        &painted_states,
                    int                                             initial_row_surfaces = 0);

    // Valid once ShowModal() returned wxID_OK. The asset, if the user picked a new image, is
    // already in the store this was constructed with.
    //
    // It RE-READS the controls before answering rather than trusting what the last change event
    // cached. The events are still what drives the preview, but Apply must be the widgets' own
    // answer: a control whose change event never arrived (a platform that does not send one for
    // a keyboard-driven wxChoice, a value typed into the spin control and committed by pressing
    // Apply) would otherwise apply the previous setting while the dialog showed the new one -
    // which is exactly the shape of "I chose Wrapped and got Flat".
    ImageFillParams params();

    // Which "Nozzle-resolution colour" boxes are ticked (Phase 5a: "Top surfaces" and "Side
    // walls"), as a bitmask of IMAGE_FILL_SURFACE_TOP / _WALLS; params().surfaces carries the same
    // value. Plater::apply_image_fill() reads this AFTER params() to decide whether to create or
    // update the part's ImageWeighted MixedFilament row and which keys to bind it to, or to drop
    // the part's bindings. Phase 2's facet painting (image_fill_apply(), driven by params()) always
    // happens regardless - see the boxes' own tooltips in the .cpp for why.
    int image_row_surfaces() const;

    // Read an image file into the store (as the "Choose image..." button does), so the dialog can
    // start with an image dropped onto the 3D scene. Returns the asset id, empty on failure.
    static std::string add_image_file(const wxString &path, ImageAssetStore &assets);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    void collect();          // widgets -> m_params
    void refresh_preview();  // m_params -> the thumbnail and the summary line
    void on_pick_image();

    ImageFillParams                                m_params;
    ImageAssetStore                               *m_assets = nullptr;
    const indexed_triangle_set                    &m_mesh;
    const TriangleSelector::TriangleSplittingData &m_existing;
    std::vector<ImageFillFilament>                 m_filaments;
    std::vector<int>                               m_painted_states;
    std::string                                    m_image_label;

    wxStaticText     *m_image_text      = nullptr;
    wxChoice         *m_source_choice   = nullptr;   // image / two-stop / three-stop gradient
    wxChoice         *m_projection      = nullptr;
    wxChoice         *m_axis            = nullptr;
    wxChoice         *m_faces           = nullptr;   // facing / through
    wxCheckBox       *m_from_negative   = nullptr;   // the -axis side, or inwards for a wrap
    wxCheckBox       *m_box_mirror      = nullptr;   // Box only: read the right way round outside
    wxChoice         *m_selection       = nullptr;   // whole part, or one painted state
    wxCheckBox       *m_flip_u          = nullptr;
    wxCheckBox       *m_flip_v          = nullptr;
    wxCheckListBox   *m_filament_list   = nullptr;
    wxSpinCtrlDouble *m_detail          = nullptr;
    wxCheckBox       *m_row_top         = nullptr;   // "Nozzle-resolution colour: Top surfaces"
    wxCheckBox       *m_row_walls       = nullptr;   // "Nozzle-resolution colour: Side walls"
    wxChoice         *m_wall_method     = nullptr;   // Phase 5b: colour runs / layer stripes
    wxSpinCtrl       *m_stripe_depth    = nullptr;   // Phase 5b: width swing, percent
    // Phase 5c: where the picture sits.
    wxSpinCtrlDouble *m_offset_u        = nullptr;   // percent of the box
    wxSpinCtrlDouble *m_offset_v        = nullptr;
    wxSpinCtrlDouble *m_scale           = nullptr;   // percent of the box
    wxSpinCtrlDouble *m_rotation        = nullptr;   // degrees
    wxCheckBox       *m_keep_aspect     = nullptr;
    wxCheckBox       *m_tile            = nullptr;
    wxStaticBitmap   *m_preview         = nullptr;
    wxStaticText     *m_summary         = nullptr;
    wxButton         *m_ok              = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageFillDialog_hpp_
