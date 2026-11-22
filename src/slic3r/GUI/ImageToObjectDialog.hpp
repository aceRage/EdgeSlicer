#ifndef slic3r_GUI_ImageToObjectDialog_hpp_
#define slic3r_GUI_ImageToObjectDialog_hpp_

// "Image to object": a printable object from the outline of a PNG / JPG image.
// Design: tests/design_image_to_objects.md. Phase 1 makes a cookie cutter.
// The geometry is created in libslic3r/OutlineObjects.

#include "GUI_Utils.hpp"

#include "libslic3r/ImageTrace.hpp"
#include "libslic3r/OutlineObjects.hpp"

#include <string>
#include <vector>

#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSlider;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

// Everything the user sets in the dialog
struct ImageToObjectSettings
{
    // How the shape is read from the image
    TraceShapeFrom shape_from     = TraceShapeFrom::Auto;
    int            threshold      = 128;
    bool           auto_threshold = true;
    bool           invert         = false;
    double         blur           = 1.;

    SilhouetteParams   silhouette;
    CookieCutterParams cutter;

    ImageToObjectSettings() { silhouette.mirror = true; }
};

class ImageToObjectDialog : public DPIDialog
{
public:
    // image_name .. file name without extension, used as name of the object
    ImageToObjectDialog(wxWindow *parent, const TraceImage &image, const std::string &image_name, const ImageToObjectSettings &settings);
    ~ImageToObjectDialog() override;

    // Valid after ShowModal() returns wxID_OK
    const OutlineObjectResult &result() const { return m_result; }
    std::string                object_name() const;

    // Last used values
    static ImageToObjectSettings load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    ImageToObjectSettings collect() const;
    void                  update_enabled();
    void                  schedule_preview();
    void                  refresh_preview();
    void                  refresh_source_preview();
    void                  change_image();
    bool                  finish();
    void                  save_to_config() const;
    // Trace the image and clean the outline. threshold .. the one used by the trace, -1 = none
    Silhouette            make_shape(const TraceImage &image, const ImageToObjectSettings &settings, int max_side, std::string &error,
                                     int *threshold = nullptr) const;

    TraceImage            m_image;
    TraceImage            m_preview_image; // downscaled for fast preview
    std::string           m_image_name;
    ImageToObjectSettings m_settings;
    OutlineObjectResult   m_result;
    wxTimer               m_timer;

    wxStaticText     *m_image_label    = nullptr;
    wxButton         *m_change_image   = nullptr;
    wxChoice         *m_type           = nullptr;
    wxChoice         *m_shape_from     = nullptr;
    wxSlider         *m_threshold      = nullptr;
    wxCheckBox       *m_auto_threshold = nullptr;
    wxCheckBox       *m_invert         = nullptr;
    wxSpinCtrlDouble *m_blur           = nullptr;
    wxSpinCtrlDouble *m_size           = nullptr;
    wxSpinCtrlDouble *m_close_gaps     = nullptr;
    wxSpinCtrlDouble *m_min_feature    = nullptr;
    wxCheckBox       *m_fill_holes     = nullptr;
    wxCheckBox       *m_largest_only   = nullptr;
    wxCheckBox       *m_mirror         = nullptr;
    wxSpinCtrlDouble *m_blade_height   = nullptr;
    wxSpinCtrlDouble *m_blade_thickness = nullptr;
    wxSpinCtrlDouble *m_handle_width   = nullptr;
    wxSpinCtrlDouble *m_handle_height  = nullptr;
    wxCheckBox       *m_sharp_edge     = nullptr;
    wxChoice         *m_braces         = nullptr;

    wxStaticBitmap *m_source_preview = nullptr;
    wxStaticBitmap *m_preview_bitmap = nullptr;
    wxStaticText   *m_info           = nullptr;
    wxStaticText   *m_warning        = nullptr;
    wxButton       *m_ok             = nullptr;

    std::vector<wxWindow *> m_brightness; // only for brightness threshold
};

// Ask for an image when the path is empty, show the dialog and add the object to the plate.
// False when canceled or failed.
bool create_object_from_image(const wxString &image_path = wxEmptyString);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageToObjectDialog_hpp_
