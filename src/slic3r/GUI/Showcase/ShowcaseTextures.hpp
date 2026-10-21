#ifndef slic3r_GUI_ShowcaseTextures_hpp_
#define slic3r_GUI_ShowcaseTextures_hpp_

// Showcase tab, phase S2: the bundled assets as the GUI uses them - the environment and floor
// manifests (read once from resources), the GL uploads of an environment atlas (a mipmapped cube
// map, one roughness per level) and of a floor or backdrop picture (mipmapped 2D) - and what each
// filament slot of the project looks like (colour, preset name and type, the finish D4 gives it).

#include "ShowcaseRenderer.hpp"   // ShowcaseImage

#include "libslic3r/Showcase/ShowcaseAssets.hpp"
#include "libslic3r/Showcase/ShowcaseMaterial.hpp"

#include <string>
#include <vector>

#include <wx/string.h>

namespace Slic3r {
namespace GUI {

// Parsed on first use; empty manifests when the files are missing (the tab then offers no
// environments or photo floors, nothing fails).
const Showcase::EnvironmentManifest &showcase_environments();
const Showcase::FloorManifest       &showcase_floors();
// <resources>/<ENVIRONMENTS_DIR or FLOORS_DIR>/<file>
std::string showcase_environment_path(const Showcase::EnvironmentInfo &env);
std::string showcase_floor_path(const Showcase::FloorTextureInfo &floor);
// The translated name of a bundled environment or floor (known ids), else the manifest's.
wxString    showcase_environment_name(const Showcase::EnvironmentInfo &env);
wxString    showcase_floor_name(const Showcase::FloorTextureInfo &floor);
// The translated name of a finish.
wxString    showcase_finish_name(Showcase::Finish finish);

// Decodes an image file (anything wxImage reads) into RGBA, top row first.
bool showcase_load_rgba_file(const std::string &path, ShowcaseImage &out);

// GL (the canvas' context current). Each returns a new texture, 0 on failure; the caller deletes it.
// A 2D picture, mipmapped, repeating or clamped.
unsigned int showcase_upload_texture(const ShowcaseImage &image, bool repeat);
// An environment atlas (layout of Showcase::env_atlas_face) as a cube map with `levels` mips.
unsigned int showcase_upload_environment(const ShowcaseImage &atlas, int face_size, int levels);

// One filament slot of the project (1-based `slot`).
struct ShowcaseFilament
{
    int              slot = 1;
    std::string      preset_name;
    std::string      type;
    std::string      color;          // "#RRGGBB"
    Showcase::Finish inferred = Showcase::Finish::Standard;
};
std::vector<ShowcaseFilament> showcase_filaments();

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseTextures_hpp_
