#ifndef slic3r_Showcase_ShowcaseBlender_hpp_
#define slic3r_Showcase_ShowcaseBlender_hpp_

// Showcase realism phase 4 (tests/design_showcase_realism.md 5): the Blender package, format
// "edgeslicer-blender-package" version 1. EdgeSlicer writes it ("Export for Blender...", "Open in
// Blender"); the add-on in resources/blender/edgeslicer_render reads it and builds a Cycles scene.
//
//   scene.glb          the plate's parts (model mode: one node per part and filament) or the toolpath
//                      tubes (one node per filament), glTF Y-up in metres; materials core PBR + KHR
//                      extensions, our parameters in each material's extras ("edgeslicer")
//   showcase.json      everything else, in Blender's frame (Z up, metres): camera, lights,
//                      environment, background, floor, materials by filament, output, look
//   camera_path.json   the video's camera per frame (and the build-up height, the turntable's spin)
//   environment/       the HDRI (copied, or written from a procedural / bundled one)
//   textures/          the backplate, the floor picture
//   overlays/          the callouts and the watermark as one transparent PNG (stills, D-R18)
//   README.txt
//
// Units: EdgeSlicer works in millimetres with Z up; everything in the package is metres (x 0.001),
// glTF positions with Y up ((x, y, z) mm -> (x, z, -y) m, which Blender's importer turns back).
//
// Building the package is pure and deterministic (the same input gives the same bytes); writing a
// folder or a zip is separate. GL- and wxWidgets-free.

#include "ShowcaseGltf.hpp"
#include "ShowcaseHdr.hpp"
#include "ShowcaseMaterial.hpp"
#include "ShowcaseScene.hpp"
#include "ShowcaseTubes.hpp"
#include "ShowcaseVideo.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

constexpr const char *BLENDER_PACKAGE_FORMAT      = "edgeslicer-blender-package";
constexpr int         BLENDER_PACKAGE_VERSION     = 1;
constexpr const char *BLENDER_CAMERA_PATH_FORMAT  = "edgeslicer-blender-camera-path";
// The add-on's name (module and extension id) and the oldest Blender it supports (5.4 / 5.5).
constexpr const char *BLENDER_ADDON_ID            = "edgeslicer_render";
constexpr int         BLENDER_MIN_MAJOR           = 4;
constexpr int         BLENDER_MIN_MINOR           = 2;
constexpr double      BLENDER_MM_TO_M             = 0.001;

// EdgeSlicer (Z up, mm) -> glTF (Y up, m).
std::array<float, 3>  gltf_from_mm(double x, double y, double z);
// EdgeSlicer (Z up, mm) -> Blender (Z up, m).
std::array<double, 3> blender_from_mm(const std::array<double, 3> &mm);

// "#RRGGBB" -> linear RGB (srgb_to_linear() of ShowcaseTone.hpp per channel).
std::array<float, 3> hex_to_linear(const std::string &hex, const std::array<float, 3> &fallback = { { 1.f, 1.f, 1.f } });

// --- materials -----------------------------------------------------------------------------------

// A filament's glow-in-the-dark (GlowFilament.hpp), as the package needs it.
struct BlenderGlow
{
    bool                 on = false;
    std::array<float, 3> srgb{ { 0.f, 0.f, 0.f } };   // the glow colour (vibrant() already applied)
    float                intensity = 1.f;              // the project-wide multiplier
};

// A finish mapped to Principled BSDF inputs (Blender 4.x names) plus the procedural detail the add-on
// builds with nodes. Derived from material_params() and effective_translucency(), so it follows the
// finish definitions of the live view.
struct BlenderMaterial
{
    std::string          finish;                       // to_string(Finish)
    std::array<float, 3> base_color{ { 1.f, 1.f, 1.f } };   // linear
    float                roughness           = 0.5f;
    float                metallic            = 0.f;
    float                ior                 = 1.5f;   // from the finish's F0
    float                specular_tint       = 0.f;    // 0 = white highlights .. 1 = in the base colour
    float                anisotropic         = 0.f;
    std::string          tangent             = "radial_z";   // "radial_z" (meshes) or "attribute" (_SILK_DIR, tubes)
    float                coat_weight         = 0.f;
    float                coat_roughness      = 0.05f;
    float                sheen_weight        = 0.f;
    float                sheen_roughness     = 0.5f;
    float                transmission_weight = 0.f;
    float                thickness_m         = 0.f;    // a typical wall, for the glTF volume
    float                absorption_density  = 0.f;    // per metre (Beer-Lambert), with the base colour
    float                subsurface_weight   = 0.f;
    float                subsurface_scale_m  = 0.f;
    std::array<float, 3> subsurface_radius{ { 1.f, 1.f, 1.f } };
    std::array<float, 3> emission_color{ { 0.f, 0.f, 0.f } };   // linear
    float                emission_strength   = 0.f;
    // Procedural detail (0 = none).
    float                sparkle             = 0.f;    // share of flake cells
    float                sparkle_cell_m      = 0.f;
    float                fiber_noise         = 0.f;
    std::string          speckle             = "none"; // "none", "stone", "wood"
    float                speckle_amount      = 0.f;
    float                layer_bump          = 0.f;    // bump strength of the layer lines (meshes only)
};

// `translucency`: the scene's Materials::translucency (0..1). `tubes`: toolpath beads (their own
// geometry carries the layers, and the silk tangent comes from the _SILK_DIR attribute).
// `lights_off`: the glow is shown (Materials::lights_off).
BlenderMaterial blender_material(Finish finish, const std::string &hex_color, float translucency, LayerLines layer_lines, bool tubes,
                                 const BlenderGlow &glow, bool lights_off);
// The same as glTF (the fallback when the add-on is not used) with `extras_json` as its extras.
GltfMaterial    gltf_material(const BlenderMaterial &m, const std::string &name, const std::string &extras_json);
// The material as the JSON object the add-on reads (also the glTF extras' "blender").
std::string     blender_material_json(const BlenderMaterial &m);

// --- camera and lights ---------------------------------------------------------------------------

struct BlenderCamera
{
    bool                  ortho = false;
    std::array<double, 3> location{ { 0., 0., 0. } };   // m
    std::array<double, 3> target{ { 0., 0., 0. } };     // m
    std::array<double, 3> up{ { 0., 0., 1. } };
    double                fov_y_deg    = 30.;           // perspective: the frame's vertical angle of view
    double                ortho_height = 0.1;           // ortho: the frame's height, m
    // Sensor fit VERTICAL with a full-frame-equivalent sensor of the frame's aspect (ShowcaseLens.hpp),
    // so the focal length and the f-number give the depth of field Showcase shows.
    double                sensor_height_mm = 24.;
    double                lens_mm          = 50.;
    double                clip_start = 0.001, clip_end = 1000.;   // m
};
// The camera of a pose: `frame_h_px` is the height of the safe frame its zoom is measured in.
BlenderCamera blender_camera(const CameraPose &pose, Projection projection, double frame_h_px, double aspect);

struct BlenderLight
{
    std::string           name;
    std::array<double, 3> direction{ { 0., 0., 1. } };   // TO the light, unit
    std::array<float, 3>  color{ { 1.f, 1.f, 1.f } };     // linear, largest channel 1
    float                 strength  = 1.f;                // sun irradiance (W/m2)
    float                 angle_deg = 3.f;                // the sun's apparent size (shadow softness)
};
// The key, fill and rim (three_point_lights) as suns. A diffuse white surface facing a light of
// intensity 1 in Showcase has radiance 1, which a Blender sun gives at strength pi. `view_dir` is the
// direction TO the camera (the key follows it when the light is not world-fixed). `key_from_hdri`
// leaves the key out (the HDRI's own sun lights the scene). `dim` is Materials::scene_dim().
std::vector<BlenderLight> blender_lights(const Lighting &lighting, const std::array<double, 3> &view_dir, bool key_from_hdri, float dim);

// --- the package ---------------------------------------------------------------------------------

struct PackageFilament
{
    int         slot = 1;               // 1-based
    std::string color = "#FFFFFF";
    std::string preset_name;
    std::string type;
    Finish      finish = Finish::Standard;
    BlenderGlow glow;
};

struct PackageLayers
{
    bool               valid   = false;
    bool               uniform = true;
    float              first   = 0.f;   // mm
    float              height  = 0.f;   // mm
    std::vector<float> tops;            // mm (non-uniform layers), at most 4096 kept
};

struct PackagePart
{
    std::string           name;
    int                   object_idx = -1, volume_idx = -1, instance_idx = 0;
    int                   filament = 1;   // 1-based
    std::vector<float>    vertices;       // x, y, z world mm
    std::vector<uint32_t> indices;
    PackageLayers         layers;
};

struct PackageHdri
{
    std::string          id, name, source, licence;
    std::string          ext = "hdr";      // "hdr" or "exr"
    std::vector<uint8_t> bytes;
    float                radiance_scale = 1.f;   // what Showcase multiplies the panorama by (peak diffuse 1)
    float                ground_height_mm = 1600.f, ground_radius_mm = 16000.f;
    bool                 sun_found = false;
};

struct PackageImage
{
    std::string          ext;     // "png" / "jpg"
    std::vector<uint8_t> bytes;
    int                  width = 0, height = 0;   // as shown (EXIF / the backplate's turn applied by the caller)
    bool empty() const { return bytes.empty(); }
};

struct PackageFrame
{
    CameraPose pose;
    double     build_height_mm = -1.;   // < 0: everything shown
};

struct PackageInput
{
    Scene                        scene;
    std::string                  edgeslicer_version;
    int                          width = 1920, height = 1080;   // output pixels
    double                       frame_height_px = 0.;          // the zooms' safe frame; 0 = the output height
    std::vector<PackageFilament> filaments;
    std::vector<PackagePart>     parts;
    bool                         toolpaths = false;            // the GLB holds `tubes`, not `parts`
    TubeResult                   tubes;
    std::array<double, 3>        model_min{ { 0., 0., 0. } }, model_max{ { 0., 0., 0. } };   // mm
    bool                         has_hdri = false;
    PackageHdri                  hdri;
    PackageImage                 background_image;   // Image / Backplate
    PackageImage                 floor_image;        // Texture / Image floors
    float                        floor_tile_mm   = 500.f;
    float                        floor_roughness = 0.6f;
    std::vector<uint8_t>         overlay_png;        // callouts + watermark (transparent, output size)
    std::vector<PackageFrame>    frames;             // the video, one per frame (empty = a still)
    int                          fps = 30;
    bool                         smooth_normals    = true;   // meshes: smooth within SMOOTH_ANGLE_DEG
    bool                         layer_bump        = true;   // meshes: the layer lines as a bump
    bool                         show_internals    = false;  // informational (the tubes are built already)
};

constexpr float BLENDER_SMOOTH_ANGLE_DEG = 30.f;

struct PackageFile
{
    std::string          path;   // '/' separated, relative to the package root
    std::vector<uint8_t> bytes;
};

struct PackageResult
{
    std::vector<PackageFile> files;
    size_t                   triangles = 0;
    size_t                   nodes     = 0;
    std::string              summary;   // one line for the log and the status bar
};

bool build_blender_package(const PackageInput &in, PackageResult &out, std::string *error = nullptr);

// Writes the files under `dir` (made if needed). Never throws.
bool write_package_folder(const std::vector<PackageFile> &files, const std::string &dir, std::string *error = nullptr);
// One zip of the files (stored deflated, fixed time stamps: the same files give the same zip).
bool write_package_zip(const std::vector<PackageFile> &files, const std::string &zip_path, std::string *error = nullptr);
// The files of `folder` (recursively, sorted, skipping __pycache__ and *.pyc) under `prefix/` in a zip:
// the add-on's installable zip.
bool zip_folder(const std::string &folder, const std::string &prefix, const std::string &zip_path, std::string *error = nullptr);

// A mesh with normals: vertices whose faces meet at less than `angle_deg` share a smooth normal, the
// others are split (the auto-smooth a CAD export gets). Positions in, positions out.
struct NormalMesh
{
    std::vector<float>    positions;
    std::vector<float>    normals;
    std::vector<uint32_t> indices;
};
NormalMesh mesh_with_normals(const std::vector<float> &vertices, const std::vector<uint32_t> &indices, float angle_deg);

// A bundled (pre-filtered) environment atlas's sharpest level as an equirectangular panorama, `width`
// pixels wide: what a non-HDRI environment gives Blender to light with.
bool atlas_to_equirect(const std::vector<uint8_t> &rgba, int atlas_w, int atlas_h, int face_size, int levels, float rgbm_range, int width,
                       HdrImage &out);

std::string package_readme(const std::string &edgeslicer_version);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseBlender_hpp_
