#ifndef slic3r_Showcase_ShowcaseAssets_hpp_
#define slic3r_Showcase_ShowcaseAssets_hpp_

// Showcase tab, phase S2: the bundled, read-only assets (D3) - pre-filtered environment maps for
// image-based lighting and the photo floor textures - as described by their manifests:
//
//   resources/textures/showcase/environments/environments.json   (+ <id>.png atlases)
//   resources/textures/showcase/floors/floors.json               (+ <id>.jpg tiles)
//
// The atlases are baked offline by scripts/showcase/bake_showcase_assets.py (README.md next to
// them lists the CC0 sources). Each holds a GGX-prefiltered cube map, one roughness level per mip:
// level L is a band of six faces (+X, -X, +Y, -Y, +Z, -Z in GL cube space) of face_size >> L
// pixels, the bands stacked from the top of the image. Texels are RGBM (rgb * a * rgbm_range,
// linear). Diffuse light is nine SH coefficients per map in the manifest.
//
// GL- and wxWidgets-free: the renderer decodes the files, the unit tests check the manifests.

#include "ShowcaseScene.hpp"

#include <array>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// Folders relative to resources_dir().
inline const char *ENVIRONMENTS_DIR = "textures/showcase/environments";
inline const char *FLOORS_DIR       = "textures/showcase/floors";
inline const char *ENVIRONMENTS_MANIFEST = "environments.json";
inline const char *FLOORS_MANIFEST       = "floors.json";

struct EnvironmentInfo
{
    std::string id;
    std::string name;     // English; the GUI translates known ids itself
    std::string file;     // atlas, relative to the manifest
    // Lambert outgoing radiance for albedo 1 as L2 spherical harmonics of the WORLD normal (Z up):
    // sum sh[i] * Y_i(n), Y in the order of sh_basis().
    std::array<std::array<float, 3>, 9> sh{};
    bool                  has_sun = false;
    std::array<float, 3>  sun_dir{ { 0.f, 0.f, 1.f } };   // the dominant light, world, unit
    std::string           source;
    std::string           authors;
    std::string           licence;
};

struct EnvironmentManifest
{
    int                          rgbm_range = 16;
    int                          face_size  = 128;
    int                          levels     = 5;
    std::vector<EnvironmentInfo> environments;

    const EnvironmentInfo *find(const std::string &id) const;
};

struct FloorTextureInfo
{
    std::string id;
    std::string name;
    std::string file;
    float       tile_mm   = 500.f;   // the tile's real-world size
    float       roughness = 0.6f;
    std::string source;
    std::string authors;
    std::string licence;
};

struct FloorManifest
{
    std::vector<FloorTextureInfo> floors;
    const FloorTextureInfo *find(const std::string &id) const;
};

// Never throw. False (and `out` empty) for text that is not a usable manifest; entries without an
// id or a file, or with a file name that leaves the folder, are skipped.
bool parse_environment_manifest(const std::string &text, EnvironmentManifest &out, std::string *error = nullptr);
bool parse_floor_manifest(const std::string &text, FloorManifest &out, std::string *error = nullptr);

// Atlas layout: the size of the whole image and the rectangle of one face of one level (origin top
// left). Empty for a level or face out of range.
int   env_atlas_width(int face_size);
int   env_atlas_height(int face_size, int levels);
RectI env_atlas_face(int face_size, int levels, int level, int face);

// One RGBM texel back to linear RGB.
std::array<float, 3> decode_rgbm(uint8_t r, uint8_t g, uint8_t b, uint8_t a, float range);

// The nine real SH basis functions at a unit direction, and the diffuse light they describe.
std::array<float, 9> sh_basis(const std::array<float, 3> &n);
std::array<float, 3> eval_sh(const std::array<std::array<float, 3>, 9> &sh, const std::array<float, 3> &n);

// The GL cube map direction of a world direction (world is Z up, GL cube space Y up), as the
// shader samples it; and back.
std::array<float, 3> world_to_cube(const std::array<float, 3> &w);
std::array<float, 3> cube_to_world(const std::array<float, 3> &c);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseAssets_hpp_
