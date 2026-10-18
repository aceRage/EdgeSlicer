#ifndef slic3r_Showcase_ShowcaseScene_hpp_
#define slic3r_Showcase_ShowcaseScene_hpp_

// Showcase (tab after Preview): presentation renders of the model. This is the GL-free half - the
// scene description, its JSON form in the 3MF (Metadata/showcase/scenes.json), the built-in presets
// and the pure layout maths the renderer and the exporter share (export sizes, the safe frame, the
// user's branding watermark). Design: tests/design_showcase_tab.md (phase S1).
//
// Kept wxWidgets- and GL-free so the 3MF code, the CLI and the unit tests can use it.

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
namespace Showcase {

// The scenes.json schema this build writes. A file with a higher number opens read-only: its scenes
// are shown but the file is written back exactly as it was read.
constexpr int SCHEMA_VERSION = 1;

// The archive folder. Nothing in the core model references it, so other readers ignore it.
inline const char *ARCHIVE_DIR        = "Metadata/showcase/";
inline const char *ARCHIVE_SCENES     = "Metadata/showcase/scenes.json";

// Limits (D11): no side of an export, nor of the supersampled image behind it, exceeds this.
constexpr int MAX_EXPORT_SIDE = 8192;
constexpr int MIN_EXPORT_SIDE = 16;

enum class BackgroundType : int { Solid = 0, VerticalGradient, RadialGradient, Image, Transparent };
enum class ImageFit       : int { Fill = 0, Fit, Tile };
enum class FloorType      : int { None = 0, ShadowCatcher, Solid, Grid };
enum class Quality        : int { Draft = 0, High, Best };
enum class ImageFormat    : int { PNG = 0, JPEG };
enum class Projection     : int { Perspective = 0, Ortho };
enum class Anchor         : int { TopLeft = 0, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };

// The built-in presets of S1. More arrive with the lighting work (S2).
enum class PresetId : int { StudioWhite = 0, DarkGradient, BuildPlate };

const char *to_string(BackgroundType v);
const char *to_string(ImageFit v);
const char *to_string(FloorType v);
const char *to_string(Quality v);
const char *to_string(ImageFormat v);
const char *to_string(Projection v);
const char *to_string(Anchor v);
const char *to_string(PresetId v);
bool from_string(const std::string &s, BackgroundType &v);
bool from_string(const std::string &s, ImageFit &v);
bool from_string(const std::string &s, FloorType &v);
bool from_string(const std::string &s, Quality &v);
bool from_string(const std::string &s, ImageFormat &v);
bool from_string(const std::string &s, Projection &v);
bool from_string(const std::string &s, Anchor &v);
bool from_string(const std::string &s, PresetId &v);

// "#RRGGBB" (or "#RRGGBBAA") <-> 0..1 floats. Colours are kept as text in the scene so a save and a
// reload give back exactly the same string.
bool               parse_hex_color(const std::string &hex, std::array<float, 4> &rgba);
std::string        normalize_hex_color(const std::string &hex, const std::string &fallback);
std::string        to_hex_color(const std::array<float, 4> &rgba, bool with_alpha = false);

struct Background
{
    BackgroundType type   = BackgroundType::RadialGradient;
    std::string    color1 = "#FFFFFF";   // solid colour; gradient top / centre
    std::string    color2 = "#D9DCE1";   // gradient bottom / edge
    std::string    image;                // sha256 of an image in Project::images (type Image)
    ImageFit       fit    = ImageFit::Fill;

    bool operator==(const Background &o) const;
    bool operator!=(const Background &o) const { return !(*this == o); }
};

struct Floor
{
    FloorType   type            = FloorType::ShadowCatcher;
    std::string color           = "#F2F2F2";
    float       shadow_strength = 0.45f;   // 0..1, how dark a full shadow gets on the floor
    float       grid_spacing    = 10.f;    // mm, Grid floor
    bool        hide_plate      = true;    // draw the floor instead of the printer's plate
    bool        hide_wipe_tower = true;

    bool operator==(const Floor &o) const;
    bool operator!=(const Floor &o) const { return !(*this == o); }
};

struct Lighting
{
    // D14: the key light is fixed in the world, so orbiting the camera leaves the shadows where they
    // are. Off = the editing views' headlight.
    bool  world_fixed   = true;
    // Direction TO the light: azimuth around +Z from +X (counter-clockwise, degrees), elevation above
    // the XY plane (degrees).
    float azimuth_deg   = 225.f;
    float elevation_deg = 55.f;
    bool  shadows       = true;
    float shadow_intensity = 0.35f;   // darkening of a fully shadowed object surface
    bool  ssao          = true;

    bool operator==(const Lighting &o) const;
    bool operator!=(const Lighting &o) const { return !(*this == o); }
};

// The direction TO the light for the given angles, world space, unit length.
std::array<double, 3> light_direction(float azimuth_deg, float elevation_deg);

struct CameraView
{
    std::string           name;
    Projection            projection = Projection::Perspective;
    std::array<double, 3> eye{ 0., -200., 150. };
    std::array<double, 3> target{ 0., 0., 0. };
    std::array<double, 3> up{ 0., 0., 1. };
    double                zoom  = 1.;
    bool                  valid = false;   // false until the user (or the first frame) sets it

    bool operator==(const CameraView &o) const;
    bool operator!=(const CameraView &o) const { return !(*this == o); }
};

// D12 (owner, 2026-10-18): the user's own branding, a logo and/or a line of text over the image.
// Off by default; never a forced "Made with ..." mark.
struct Watermark
{
    std::string name;                 // the preset name it came from (informational)
    bool        enabled     = false;
    std::string text;
    std::string font_face;            // empty = the UI font
    bool        bold        = false;
    std::string text_color  = "#FFFFFF";
    std::string logo;                 // sha256 of an image in Project::images, empty = no logo
    Anchor      anchor      = Anchor::BottomRight;
    float       size_pct    = 6.f;    // height of the mark, % of the image height
    float       opacity     = 0.85f;  // 0..1
    float       margin_pct  = 3.f;    // distance from the edges, % of the shorter image side

    bool has_content() const { return !text.empty() || !logo.empty(); }
    bool operator==(const Watermark &o) const;
    bool operator!=(const Watermark &o) const { return !(*this == o); }
};

struct ExportSettings
{
    int         width        = 1920;
    int         height       = 1080;
    Quality     quality      = Quality::High;
    ImageFormat format       = ImageFormat::PNG;
    int         jpeg_quality = 92;
    Watermark   watermark;

    bool operator==(const ExportSettings &o) const;
    bool operator!=(const ExportSettings &o) const { return !(*this == o); }
};

struct Scene
{
    std::string             id;
    std::string             name;
    int                     plate_index = 0;      // 0-based, D17: one scene per plate
    std::string             source      = "model"; // "model" | "toolpaths" (S5)
    std::string             preset_base;           // the preset it started from (to_string(PresetId))
    Background              background;
    Floor                   floor;
    Lighting                lighting;
    CameraView              camera;
    std::vector<CameraView> views;                  // saved, named views
    ExportSettings          export_settings;

    // Keys this build does not know, as a JSON object text, written back on save (forward compat).
    std::string             extra_json;

    // The image hashes this scene points at.
    std::vector<std::string> used_images() const;

    // Same content, ignoring extra_json.
    bool operator==(const Scene &o) const;
    bool operator!=(const Scene &o) const { return !(*this == o); }
};

struct ImageBlob
{
    std::string          ext;     // "png" or "jpg"
    std::vector<uint8_t> bytes;   // encoded, verbatim
    bool operator==(const ImageBlob &o) const { return ext == o.ext && bytes == o.bytes; }
};

class Project
{
public:
    int                schema = SCHEMA_VERSION;
    std::vector<Scene> scenes;
    std::string        active;   // id of the scene last shown
    // sha256 -> image. Only images a scene points at are written.
    std::map<std::string, ImageBlob> images;
    std::string        extra_json;   // unknown top-level keys, written back

    bool empty() const { return scenes.empty(); }
    void clear();

    // A newer schema than this build knows: shown, never rewritten.
    bool        read_only() const { return schema > SCHEMA_VERSION; }
    // The scenes.json text a read-only project was loaded from, written back verbatim.
    std::string original_text;

    Scene       *scene_for_plate(int plate_index);
    const Scene *scene_for_plate(int plate_index) const;
    // The plate's scene, made from `preset` if it has none yet.
    Scene       &ensure_scene_for_plate(int plate_index, PresetId preset = PresetId::StudioWhite);

    // Stores the bytes under their SHA-256 and returns it. `ext` is "png" or "jpg".
    std::string  add_image(std::vector<uint8_t> bytes, const std::string &ext);
    const ImageBlob *find_image(const std::string &sha) const;
    // Images some scene uses, sorted.
    std::vector<std::string> used_images() const;
    // Drops images no scene uses.
    void         prune_images();

    // A plate was deleted: its scene goes, later plates move down one.
    void         on_plate_deleted(int plate_index);

    bool operator==(const Project &o) const;
    bool operator!=(const Project &o) const { return !(*this == o); }
};

// scenes.json <-> Project (images are separate archive files). Parsing never throws: invalid text
// gives false and leaves `out` empty, and the caller loads the project without scenes.
std::string scenes_to_json(const Project &project);
bool        scenes_from_json(const std::string &text, Project &out, std::string *error = nullptr);
std::string scene_to_json(const Scene &scene);
bool        scene_from_json(const std::string &text, Scene &out);

// "Metadata/showcase/<sha>.<ext>" and back. Returns false for any other name.
std::string image_archive_path(const std::string &sha, const std::string &ext);
bool        parse_image_archive_path(const std::string &path, std::string &sha, std::string &ext);

// A fresh scene for a plate from a built-in preset.
Scene make_scene(PresetId preset, int plate_index);
// Applies the preset's look (background, floor, lighting) and keeps camera, views and export.
void  apply_preset(Scene &scene, PresetId preset);
std::vector<PresetId> builtin_presets();

// --- export planning ------------------------------------------------------------------------

// What an export of w x h at `quality` actually renders. SSAA renders the image `ssaa` times larger
// and box-filters it down; MSAA comes on top. Each internal side is capped at MAX_EXPORT_SIDE, and
// the GPU memory estimate at `budget_bytes` (1 GB by default, design section 6): over it MSAA goes
// first, then the supersampling factor.
struct ExportPlan
{
    int    width  = 0;     // output size
    int    height = 0;
    int    ssaa   = 1;     // supersampling factor
    int    internal_width  = 0;
    int    internal_height = 0;
    int    msaa   = 0;     // samples, 0 = none
    // The SSAO kernel is a few pixels wide; scaled by this so it covers the same part of the model
    // as on screen (design 2.1, finding 2).
    float  ssao_radius_scale = 1.f;
    unsigned shadow_map_size = 4096;
    double estimated_bytes = 0.;
    bool   degraded = false;   // the requested quality did not fit
};

int        ssaa_factor(Quality q);
size_t     export_memory_estimate(int internal_w, int internal_h, int msaa);
// `reference_height` is the on-screen height of the frame the user framed the shot in (the SSAO
// radius is matched to it); `max_msaa` the driver's GL_MAX_SAMPLES.
ExportPlan plan_export(int width, int height, Quality quality, int max_msaa, int reference_height,
                       size_t budget_bytes = size_t(1) << 30, int max_texture_size = 16384);
// Clamps a requested size into [MIN_EXPORT_SIDE, MAX_EXPORT_SIDE].
int        clamp_export_side(int v);

// --- the safe frame -------------------------------------------------------------------------

struct RectI
{
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const RectI &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

// The largest rectangle of the export's aspect ratio centred in a viewport (letterbox / pillarbox),
// with `margin_px` kept free on every side.
RectI fit_frame(int viewport_w, int viewport_h, int aspect_w, int aspect_h, int margin_px = 0);

// --- the watermark --------------------------------------------------------------------------

struct WatermarkLayout
{
    RectI block;   // the whole mark, clamped into the image
    RectI logo;    // empty without a logo
    RectI text;    // empty without text
    int   font_px = 0;
};

// The font size the text is drawn at in an image `image_h` pixels high.
int watermark_font_px(const Watermark &wm, int image_h, bool has_logo);
// Lays out the mark. `logo_aspect` is the logo's width / height (0 = no logo); `text_w`/`text_h` the
// size the text measures at watermark_font_px() (0 = no text). Coordinates: origin top-left.
WatermarkLayout layout_watermark(const Watermark &wm, int image_w, int image_h, double logo_aspect, int text_w, int text_h);

// --- image helpers --------------------------------------------------------------------------

// Box-filters an RGBA8 image by an integer factor (SSAA resolve).
std::vector<uint8_t> downsample_rgba(const std::vector<uint8_t> &src, int w, int h, int factor);
// Turns bottom-up GL rows into top-down rows, in place.
void flip_rows_rgba(std::vector<uint8_t> &rgba, int w, int h);
// The renderer draws over transparent black, so a transparent export comes back premultiplied.
void unpremultiply_rgba(std::vector<uint8_t> &rgba);
// Composites premultiplied-free RGBA `src` (w x h) over `dst` at (x, y), scaled by `opacity`.
void blend_over_rgba(std::vector<uint8_t> &dst, int dst_w, int dst_h, const std::vector<uint8_t> &src, int src_w, int src_h,
                     int x, int y, float opacity);
// Flattens an RGBA image onto an opaque colour (JPEG has no alpha).
void flatten_rgba(std::vector<uint8_t> &rgba, uint8_t r, uint8_t g, uint8_t b);

} // namespace Showcase
} // namespace Slic3r

#endif // slic3r_Showcase_ShowcaseScene_hpp_
