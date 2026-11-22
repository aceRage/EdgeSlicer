#ifndef slic3r_ImageObjectRecipe_hpp_
#define slic3r_ImageObjectRecipe_hpp_

// "Image to object": what is kept with an object made from an image, so it can be edited later.
// Design: tests/design_image_to_objects.md, 3.6.
//
// The settings of the tool and a downscaled copy of the image are stored on every object the
// tool added (ModelObject::image_object_recipe) and written into the project as
// Metadata/edgeslicer_image_objects.json. Other slicers ignore the file and keep the geometry.

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "ImageTrace.hpp"
#include "OutlineObjects.hpp"

namespace Slic3r {

enum class ImageObjectType : int { CookieCutter = 0, Box, Clicker };

// The picture on the lid has at most this many colours
constexpr int IMAGE_OBJECT_MAX_COLORS = IMAGE_TRACE_MAX_COLORS;

// Everything the user sets in the "Image to object" dialog
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
    ClickerParams    clicker;
    // Colours of the picture on the lid / cap: 0 = no picture, otherwise 2 .. IMAGE_OBJECT_MAX_COLORS
    int picture_colors = 3;
    // 0 .. default, otherwise 1 based filament
    int                                      body_extruder = 0;
    std::array<int, IMAGE_OBJECT_MAX_COLORS> level_extruders{};
};

/// <summary>
/// Limit every value to the range the dialog accepts and set the values which follow from
/// other ones (mirror of the silhouette, picture on / off).
/// </summary>
void clamp_image_object_settings(ImageToObjectSettings &settings);

std::string image_object_settings_to_json(const ImageToObjectSettings &settings);

/// <summary>
/// Read the settings. Untrusted text: values out of range are limited, unknown keys are
/// skipped, missing keys keep their default.
/// </summary>
/// <returns>False when the text is not a JSON object</returns>
bool image_object_settings_from_json(const std::string &json, ImageToObjectSettings &settings);

// Largest copy of the image kept with an object [bytes of PNG]
constexpr size_t IMAGE_OBJECT_MAX_SOURCE_BYTES = size_t(4) * 1024 * 1024;

struct ImageObjectRecipe
{
    // The objects added together (box and lid) share the group: 8 to 64 hexadecimal digits
    std::string group;
    // Order of the object in its group: 0 = cutter / base / body, 1 = lid / cap
    int index = 0;
    // File name of the image without extension, used in the names of the objects
    std::string image_name;
    // image_object_settings_to_json()
    std::string settings;
    // Downscaled copy of the image (PNG), can be empty
    std::string source_png;

    bool empty() const { return group.empty(); }
    bool operator==(const ImageObjectRecipe &rhs) const
    {
        return group == rhs.group && index == rhs.index && image_name == rhs.image_name && settings == rhs.settings &&
               source_png == rhs.source_png;
    }
    bool operator!=(const ImageObjectRecipe &rhs) const { return !(*this == rhs); }

    template<class Archive> void serialize(Archive &ar) { ar(group, index, image_name, settings, source_png); }
};

bool is_valid_image_object_group(const std::string &group);
// Random group, 32 hexadecimal digits
std::string new_image_object_group();

/// <summary>
/// Content of Metadata/edgeslicer_image_objects.json.
/// </summary>
/// <param name="objects">One entry per object of the model, in the order of the model;
/// nullptr or an empty recipe for an object without one</param>
/// <returns>Empty when no object has a recipe</returns>
std::string image_object_recipes_to_json(const std::vector<const ImageObjectRecipe *> &objects);

/// <summary>
/// Read Metadata/edgeslicer_image_objects.json. Untrusted text: what does not make sense is dropped.
/// </summary>
/// <returns>1 based index of the object in the model and its recipe</returns>
std::vector<std::pair<int, ImageObjectRecipe>> image_object_recipes_from_json(const std::string &json);

} // namespace Slic3r

#endif // slic3r_ImageObjectRecipe_hpp_
