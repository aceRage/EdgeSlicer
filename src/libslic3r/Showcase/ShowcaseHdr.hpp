#ifndef slic3r_Showcase_ShowcaseHdr_hpp_
#define slic3r_Showcase_ShowcaseHdr_hpp_

// Showcase realism 1h-1 (tests/design_showcase_realism.md 2.8): HDRI environments.
//
// An HDRI is a Radiance RGBE (.hdr) equirectangular panorama, or one of the procedural panoramas
// below (stand-ins used by the tests and until the bundled photographs are approved). It is turned
// into everything the Showcase renderer already knows how to use:
//
//   * the nine diffuse SH coefficients and the prefiltered cube-map atlas of ShowcaseAssets.hpp
//     (same layout, RGBM, GGX levels), so lighting, reflections and the toolpath material need no new
//     code path;
//   * the key light: the sun (or the brightest softbox) found in the panorama - direction, colour and
//     strength - which is taken out of the image-based light so that it is not counted twice;
//   * the scale that makes the panorama's peak diffuse luminance 1.0 (like the baked environments),
//     which the renderer applies to the sharp backdrop it draws from the same panorama.
//
// Conventions are those of scripts/showcase/bake_showcase_assets.py (the Poly Haven / Blender
// equirect mapping): for a world direction w (Z up), phi = atan2(y, x), theta = asin(z),
// u = 0.5 - phi / (2 pi), v = 0.5 - theta / pi, v = 0 the top row. The image centre is world +X.
//
// GL- and wxWidgets-free: the renderer uploads the results, the unit tests check them.

#include "ShowcaseAssets.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// Folder relative to resources_dir(), and the manifest in it.
inline const char *HDRI_DIR      = "showcase/hdri";
inline const char *HDRI_MANIFEST = "hdri.json";

// Linear RGB float image, row 0 = top.
struct HdrImage
{
    int                width  = 0;
    int                height = 0;
    std::vector<float> rgb;   // width * height * 3

    bool empty() const { return width <= 0 || height <= 0 || rgb.size() != size_t(width) * size_t(height) * 3; }
};

// Largest panorama accepted (a 16k x 8k one is 1.5 GB as floats).
inline constexpr int HDR_MAX_SIDE   = 16384;
inline constexpr int HDR_MAX_PIXELS = 4096 * 2048 * 2;

// ---- Radiance RGBE -------------------------------------------------------------------------

// Decodes a Radiance file held in memory: "#?RADIANCE" or "#?RGBE", FORMAT=32-bit_rle_rgbe, a
// "-Y h +X w" (or "+Y") resolution line, then new-style RLE scanlines, flat pixels or old-style
// runs. Never throws; false (and `out` empty) for anything else, with `error` set.
bool decode_rgbe(const uint8_t *data, size_t size, HdrImage &out, std::string *error = nullptr);
bool load_hdr_file(const std::string &path, HdrImage &out, std::string *error = nullptr);
// Encodes to a Radiance file (new-style RLE scanlines when the width allows it, else flat).
std::string encode_rgbe(const HdrImage &image, bool rle = true);

// ---- Equirect mapping ----------------------------------------------------------------------

// The world direction (unit) of a point of the panorama, u/v in [0, 1] (v = 0 the top).
std::array<float, 3> equirect_direction(float u, float v);
// The panorama coordinates of a world direction (any length).
void                 direction_to_equirect(const std::array<float, 3> &dir, float &u, float &v);

// ---- Procedural panoramas ------------------------------------------------------------------

enum class ProceduralHdr : int { SkySun = 0, Studio, Interior };
const char *to_string(ProceduralHdr v);
bool        from_string(const std::string &s, ProceduralHdr &v);
// width x width/2. Deterministic. Radiance is real-world-like (a sun disc far above 1000).
HdrImage    make_procedural_hdr(ProceduralHdr kind, int width = 2048);

// ---- Sun -----------------------------------------------------------------------------------

struct HdrSun
{
    bool                 found = false;
    std::array<float, 3> dir{ { 0.f, 0.f, 1.f } };     // TO the sun, world, unit (before any environment rotation)
    std::array<float, 3> color{ { 1.f, 1.f, 1.f } };   // linear, luminance 1
    float                irradiance  = 0.f;            // luminance of the light it adds to a surface facing it (scaled image)
    float                share       = 0.f;            // that over the whole panorama's cosine-weighted light at that direction
    float                solid_angle = 0.f;            // sr
    // The key light's strength so that, with key_intensity 1, the sun lights a facing surface as the
    // panorama did: irradiance / pi, kept between 0.5 and 1.5 so a faint or an extreme sun still reads.
    float                key_strength = 1.f;
};

// ---- The bake ------------------------------------------------------------------------------

struct HdrBakeOptions
{
    float target_peak = 1.f;    // the peak luminance of the diffuse SH term, over all normals, after scaling
    bool  remove_sun  = false;  // take the sun out of the light (the key light stands in for it)
    int   face_size   = 128;    // the atlas' level-0 face (the manifest of the baked environments)
    int   levels      = 5;
};

struct HdrEnvironment
{
    float                               scale = 1.f;   // applied to the panorama's radiance
    std::array<std::array<float, 3>, 9> sh{};          // Lambert outgoing radiance, albedo 1 (scaled, sun removed if asked)
    HdrSun                              sun;           // measured on the original panorama
    int                                 face_size = 0, levels = 0;
    int                                 atlas_width = 0, atlas_height = 0;
    std::vector<uint8_t>                atlas_rgba;    // RGBM, env_atlas_face layout
};

// Never throws. False for an empty panorama or one with no light at all.
bool bake_hdr_environment(const HdrImage &image, const HdrBakeOptions &options, HdrEnvironment &out);

// Pieces of the bake, exposed for the tests.
// The 9 SH coefficients of the diffuse outgoing radiance (albedo 1) of a panorama.
std::array<std::array<float, 3>, 9> project_hdr_sh(const HdrImage &image);
// The largest luminance of the SH light over a sphere of normals.
float                               sh_peak_luminance(const std::array<std::array<float, 3>, 9> &sh);
HdrSun                              find_hdr_sun(const HdrImage &image);

// ---- Manifest ------------------------------------------------------------------------------

// An HDRI the app offers: a Radiance file in HDRI_DIR, or a procedural panorama ("procedural").
struct HdriInfo
{
    std::string   id;
    std::string   name;         // English; the GUI translates known ids itself
    std::string   file;         // relative to the manifest; empty for a procedural one
    bool          procedural = false;
    ProceduralHdr kind       = ProceduralHdr::SkySun;
    std::string   source;
    std::string   authors;
    std::string   licence;
};

struct HdriManifest
{
    std::vector<HdriInfo> hdris;
    const HdriInfo *find(const std::string &id) const;
};

// Never throw. False (and `out` empty) for text that is not a manifest; entries without an id or
// a usable file/procedural kind, or with a file name that leaves the folder, are skipped. A
// listed file is not checked for existence here (the renderer falls back with a warning).
bool parse_hdri_manifest(const std::string &text, HdriManifest &out, std::string *error = nullptr);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseHdr_hpp_
