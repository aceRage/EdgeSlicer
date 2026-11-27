#ifndef slic3r_GUI_ImageToObjectDialog_hpp_
#define slic3r_GUI_ImageToObjectDialog_hpp_

// "Image to object": a printable object from the outline of a PNG / JPG image.
// Design: tests/design_image_to_objects.md. A cookie cutter, or a box with a lid in the shape
// of the image, or a clicker around a keyboard switch. The geometry is created in
// libslic3r/OutlineObjects. The settings and a copy of the image are kept with the objects
// (libslic3r/ImageObjectRecipe), so "Edit image object" opens the dialog again.

#include "GUI_Utils.hpp"

#include "libslic3r/ImageObjectRecipe.hpp"
#include "libslic3r/ImageTrace.hpp"
#include "libslic3r/OutlineObjects.hpp"

#include <array>
#include <string>
#include <vector>

#include <wx/gdicmn.h>
#include <wx/timer.h>

class wxButton;
class wxCheckBox;
class wxChoice;
class wxSlider;
class wxSpinCtrlDouble;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r { namespace GUI {

class ImageToObjectDialog : public DPIDialog
{
public:
    // source_png .. downscaled copy of the image which is kept with the objects for editing
    // image_name .. file name without extension, used in the names of the objects
    // is_edit    .. the objects replace the ones the dialog was opened for
    ImageToObjectDialog(wxWindow                    *parent,
                        const TraceImage            &image,
                        const std::string           &source_png,
                        const std::string           &image_name,
                        const ImageToObjectSettings &settings,
                        bool                         is_edit = false);
    ~ImageToObjectDialog() override;

    // Valid after ShowModal() returns wxID_OK
    const std::vector<OutlineObject> &objects() const { return m_objects; }
    const std::vector<std::string>   &warnings() const { return m_warnings; }
    const ImageToObjectSettings      &settings() const { return m_settings; }
    const std::string                &source_png() const { return m_source_png; }
    const std::string                &image_name() const { return m_image_name; }
    // The result is the fit test piece, which is not made from the image
    bool                              is_fit_test() const { return m_is_fit_test; }
    // 0 .. default, otherwise 1 based filament of the part
    int extruder(const OutlinePart &part) const;

    // Last used values
    static ImageToObjectSettings load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    // Outline (and colour levels) traced from the image
    struct Shape
    {
        Silhouette silhouette;
        // Colour levels in the frame of the silhouette, empty without a picture
        std::vector<ExPolygons>             detail;
        std::vector<std::array<uint8_t, 3>> colors;
        // Threshold used by the trace, -1 = none
        int         threshold = -1;
        std::string error;
    };

    ImageToObjectSettings collect() const;
    void                  update_enabled();
    void                  update_level_rows(const Shape &shape);
    // "Match filaments" menu: add filaments of the picture's colours, recolour or use the nearest
    void                  show_match_menu();
    void                  match_filaments(TraceFilamentMatch mode);
    void                  refill_filament_choices();
    void                  schedule_preview();
    void                  refresh_preview();
    void                  refresh_preview_unchecked();
    // The objects of the shape. False with the error set when they could not be created.
    bool                  generate(const Shape &shape, const ImageToObjectSettings &settings, std::vector<OutlineObject> &objects,
                                   std::vector<std::string> &warnings, std::string &error) const;
    bool                  view_3d() const;
    // Draw the objects of the preview as they are turned now. False when there is nothing to draw.
    bool                  render_3d();
    // The geometry failed in an unexpected way: say so in the dialog instead of ending the application
    void                  show_failure(const std::string &what);
    void                  refresh_source_preview();
    void                  change_image();
    bool                  finish();
    bool                  finish_unchecked();
    // Replace the result by the fit test piece of the clicker
    bool                  finish_fit_test();
    void                  save_to_config() const;
    Shape                 make_shape(const TraceImage &image, const ImageToObjectSettings &settings, int max_side) const;
    bool                  with_picture(const ImageToObjectSettings &settings) const;
    std::string           name_of(const std::string &what) const;
    // "#RRGGBB" of the filament, empty for the default one
    std::string           filament_color(int extruder) const;

    TraceImage                 m_image;
    std::string                m_source_png;
    bool                       m_is_edit = false;
    TraceImage                 m_preview_image; // downscaled for fast preview
    std::string                m_image_name;
    ImageToObjectSettings      m_settings;
    std::vector<OutlineObject> m_objects;
    std::vector<std::string>   m_warnings;
    std::vector<std::string>   m_filament_colors;
    // filament of the level was chosen by the user
    std::array<bool, IMAGE_OBJECT_MAX_COLORS> m_user_filament{};
    wxTimer                    m_timer;

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
    // cookie cutter
    wxSpinCtrlDouble *m_blade_height    = nullptr;
    wxSpinCtrlDouble *m_blade_thickness = nullptr;
    wxSpinCtrlDouble *m_handle_width    = nullptr;
    wxSpinCtrlDouble *m_handle_height   = nullptr;
    wxCheckBox       *m_sharp_edge      = nullptr;
    wxChoice         *m_braces          = nullptr;
    // box
    wxSpinCtrlDouble *m_box_height    = nullptr;
    wxSpinCtrlDouble *m_box_wall      = nullptr;
    wxSpinCtrlDouble *m_lid_overhang  = nullptr;
    wxSpinCtrlDouble *m_lid_clearance = nullptr;
    // clicker
    wxSpinCtrlDouble *m_body_height   = nullptr;
    wxSpinCtrlDouble *m_socket_fit    = nullptr;
    wxSpinCtrlDouble *m_stem_fit      = nullptr;
    wxSpinCtrlDouble *m_switch_x      = nullptr;
    wxSpinCtrlDouble *m_switch_y      = nullptr;
    wxSpinCtrlDouble *m_cap_overhang  = nullptr;
    wxCheckBox       *m_keyring       = nullptr;
    wxSpinCtrlDouble *m_keyring_angle = nullptr;
    wxButton         *m_fit_test      = nullptr;
    // picture on the lid of the box / on the cap of the clicker
    wxChoice         *m_picture       = nullptr;
    wxChoice         *m_body_filament = nullptr;
    std::array<wxStaticBitmap *, IMAGE_OBJECT_MAX_COLORS> m_level_swatch{};
    std::array<wxStaticText *, IMAGE_OBJECT_MAX_COLORS>   m_level_label{};
    std::array<wxChoice *, IMAGE_OBJECT_MAX_COLORS>       m_level_filament{};
    wxButton                                             *m_match_filaments = nullptr;
    // colours of the picture shown in the rows
    std::vector<std::array<uint8_t, 3>> m_level_colors;

    wxStaticBitmap *m_source_preview = nullptr;
    // "from above" (cross-sections) or the objects in 3D
    wxChoice       *m_view           = nullptr;
    wxSlider       *m_turn           = nullptr;
    wxStaticText   *m_turn_label     = nullptr;
    // objects of the 3D preview, the colours of their parts and how they are turned [degrees]
    std::vector<OutlineObject>          m_preview_objects;
    std::vector<std::array<uint8_t, 3>> m_preview_colors;
    double                              m_yaw      = -30.;
    double                              m_pitch    = 35.;
    bool                                m_dragging = false;
    wxPoint                             m_drag_from;
    wxStaticBitmap *m_preview_bitmap = nullptr;
    wxStaticText   *m_info           = nullptr;
    wxStaticText   *m_warning        = nullptr;
    wxButton       *m_ok             = nullptr;

    std::vector<wxWindow *> m_one_color;  // shape from one colour level (no picture)
    std::vector<wxWindow *> m_brightness; // only for brightness threshold
    std::vector<wxWindow *> m_cutter_controls;
    std::vector<wxWindow *> m_box_controls;
    std::vector<wxWindow *> m_clicker_controls;
    std::vector<wxWindow *> m_picture_controls; // box and clicker
    // the result is the fit test piece, not the object of the image
    bool m_is_fit_test = false;
};

// Ask for an image when the path is empty, show the dialog and add the objects to the plate.
// False when canceled or failed.
bool create_object_from_image(const wxString &image_path = wxEmptyString);

// One object made by "Image to object" is selected
bool can_edit_image_object();
// Open the dialog with the settings of the selected object and replace the object (and the
// other object made with it, e.g. the lid of a box) by the result. False when canceled or failed.
bool edit_image_object();

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageToObjectDialog_hpp_
