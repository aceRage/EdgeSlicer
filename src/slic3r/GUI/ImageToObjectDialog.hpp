#ifndef slic3r_GUI_ImageToObjectDialog_hpp_
#define slic3r_GUI_ImageToObjectDialog_hpp_

// "Image to object": a printable object from the outline of a PNG / JPG image.
// Design: tests/design_image_to_objects.md. A cookie cutter, or a box with a lid in the shape
// of the image. The geometry is created in libslic3r/OutlineObjects.

#include "GUI_Utils.hpp"

#include "libslic3r/ImageTrace.hpp"
#include "libslic3r/OutlineObjects.hpp"

#include <array>
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

enum class ImageObjectType : int { CookieCutter = 0, Box };

// The picture on the lid has at most this many colours
constexpr int IMAGE_OBJECT_MAX_COLORS = 4;

// Everything the user sets in the dialog
struct ImageToObjectSettings
{
    ImageObjectType type = ImageObjectType::CookieCutter;

    // How the shape is read from the image (when no picture is traced)
    TraceShapeFrom shape_from     = TraceShapeFrom::Auto;
    int            threshold      = 128;
    bool           auto_threshold = true;
    bool           invert         = false;
    double         blur           = 1.;

    SilhouetteParams   silhouette;
    // A cookie cutter is turned over when used
    bool               cutter_mirror = true;
    CookieCutterParams cutter;

    OutlineBoxParams box;
    // Colours of the picture on the lid: 0 = no picture, otherwise 2 .. IMAGE_OBJECT_MAX_COLORS
    int picture_colors = 3;
    // 0 .. default, otherwise 1 based filament
    int                                      body_extruder = 0;
    std::array<int, IMAGE_OBJECT_MAX_COLORS> level_extruders{};
};

class ImageToObjectDialog : public DPIDialog
{
public:
    // image_name .. file name without extension, used in the names of the objects
    ImageToObjectDialog(wxWindow *parent, const TraceImage &image, const std::string &image_name, const ImageToObjectSettings &settings);
    ~ImageToObjectDialog() override;

    // Valid after ShowModal() returns wxID_OK
    const std::vector<OutlineObject> &objects() const { return m_objects; }
    const std::vector<std::string>   &warnings() const { return m_warnings; }
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
    void                  schedule_preview();
    void                  refresh_preview();
    void                  refresh_source_preview();
    void                  change_image();
    bool                  finish();
    void                  save_to_config() const;
    Shape                 make_shape(const TraceImage &image, const ImageToObjectSettings &settings, int max_side) const;
    bool                  with_picture(const ImageToObjectSettings &settings) const;
    std::string           name_of(const std::string &what) const;
    // "#RRGGBB" of the filament, empty for the default one
    std::string           filament_color(int extruder) const;

    TraceImage                 m_image;
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
    wxChoice         *m_picture       = nullptr;
    wxChoice         *m_body_filament = nullptr;
    std::array<wxStaticBitmap *, IMAGE_OBJECT_MAX_COLORS> m_level_swatch{};
    std::array<wxStaticText *, IMAGE_OBJECT_MAX_COLORS>   m_level_label{};
    std::array<wxChoice *, IMAGE_OBJECT_MAX_COLORS>       m_level_filament{};

    wxStaticBitmap *m_source_preview = nullptr;
    wxStaticBitmap *m_preview_bitmap = nullptr;
    wxStaticText   *m_info           = nullptr;
    wxStaticText   *m_warning        = nullptr;
    wxButton       *m_ok             = nullptr;

    std::vector<wxWindow *> m_one_color;  // shape from one colour level (no picture)
    std::vector<wxWindow *> m_brightness; // only for brightness threshold
    std::vector<wxWindow *> m_cutter_controls;
    std::vector<wxWindow *> m_box_controls;
};

// Ask for an image when the path is empty, show the dialog and add the objects to the plate.
// False when canceled or failed.
bool create_object_from_image(const wxString &image_path = wxEmptyString);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ImageToObjectDialog_hpp_
