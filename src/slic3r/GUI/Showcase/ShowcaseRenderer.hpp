#ifndef slic3r_GUI_ShowcaseRenderer_hpp_
#define slic3r_GUI_ShowcaseRenderer_hpp_

// Showcase tab (phases S1 to S4): draws the Showcase canvas and renders still images from it.
// S3 adds the ID pass the filament callouts are anchored with, and their overlay on the live view.
// S4 renders video frames through the same export, keeping the off-screen buffers between frames.
//
// One drawing function serves the live view and the export (design 2.2): backdrop, the printer's
// plate or a floor, the shadow map with a world-fixed light (D14), the objects of the current
// plate, and SSAO. S2 draws the objects with the filament materials (showcase_material: finishes,
// the three-point lights, image-based light from a bundled environment, procedural layer lines)
// and gives the floor photo textures, a contact shadow and a mirror pass; without that optional
// shader the objects go through the realistic view's Phong shader as in S1. The export draws the same frame into
// an off-screen framebuffer at up to 8192 px a side (D11), supersampled and multisampled, with the
// SSAO kernel scaled to the export size, and reads it back.
//
// Realism 1a (tests/design_showcase_realism.md 2.1): the lit objects are drawn into a floating-point
// (RGBA16F) layer as linear light, and one post pass (showcase_post.fs) applies the exposure, the
// tone curve and the sRGB encoding and lays them over the backdrop, plate and floor, which stay
// display-referred in the RGBA8 scene buffer. The live view renders off-screen the same way. Without
// float buffers or the post shader, everything is drawn as before (each shader tone-maps itself).
//
// A friend of GLCanvas3D: it drives that canvas's realistic-view passes (shadows, SSAO) through
// GLCanvas3D::set_render_settings_override(), never through the user's preferences.

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/RealisticRenderSettings.hpp"
#include "libslic3r/Showcase/ShowcaseAssets.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

class wxMouseEvent;

namespace Slic3r {
namespace GUI {

struct Camera; // a struct in Camera.hpp: MSVC mangles class and struct differently (LNK2001)
class GLCanvas3D;
class GLModel;
struct ShowcaseFramebuffers;

// An RGBA image, top row first, straight (not premultiplied) alpha.
struct ShowcaseImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
};

// S3: the plate's objects drawn once in flat colours, one per filament region (design 3.3, the
// "ID pass"), over exactly the safe frame. Callout anchors are found in it, re-anchoring picks in
// it, and it tells which anchors something else hides.
struct ShowcaseIdPass
{
    struct Entry
    {
        int                  object_idx   = -1;
        int                  volume_idx   = -1;
        int                  instance_idx = 0;
        Showcase::BaseRegion base;
    };
    int                width  = 0;
    int                height = 0;
    std::vector<int>   ids;     // index into entries per pixel, -1 = nothing; top row first
    std::vector<float> depth;   // window depth 0..1; top row first
    std::vector<Entry> entries;
    Matrix4d           view_projection = Matrix4d::Identity();   // world -> clip of the safe frame

    bool         valid() const { return width > 0 && height > 0 && ids.size() == size_t(width) * size_t(height); }
    const Entry *entry_at(int x, int y) const;
    // The world point seen at a pixel; false where there is no object.
    bool         world_at(int x, int y, Vec3d &out) const;
};

class ShowcaseRenderer
{
public:
    explicit ShowcaseRenderer(GLCanvas3D &canvas);
    ~ShowcaseRenderer();
    ShowcaseRenderer(const ShowcaseRenderer &) = delete;
    ShowcaseRenderer &operator=(const ShowcaseRenderer &) = delete;

    // What to draw. The scene and the project outlive every call (they belong to the Model or to
    // the view). A null scene draws nothing but the editing view's background.
    void set_scene(const Showcase::Scene *scene, const Showcase::Project *project);
    // The backdrop or floor picture changed: upload it again on the next frame.
    void invalidate_images() { m_bg_image_key.clear(); m_floor_key.clear(); }
    // The objects or the print settings may have changed: the layer lines are worked out again.
    void invalidate_layers()
    {
        m_layers_valid     = false;
        m_model_tops_valid = false;
    }

    // The watermark preview over the live view. Called with the safe frame's size; returns the mark
    // for an image of that size, or an empty image. `version` changes whenever the mark does.
    using WatermarkProvider = std::function<bool(int frame_w, int frame_h, ShowcaseImage &mark, Showcase::RectI &where)>;
    void set_watermark_provider(WatermarkProvider provider) { m_watermark_provider = std::move(provider); }
    void invalidate_watermark() { ++m_watermark_version; }

    // S3: the callouts over the live view, drawn on the CPU (D6) into a picture of the safe frame's
    // size. Asked again whenever `version` or the camera changes.
    using OverlayProvider = std::function<bool(int frame_w, int frame_h, ShowcaseImage &image)>;
    void set_callout_provider(OverlayProvider provider, std::function<uint64_t()> version)
    {
        m_callout_provider = std::move(provider);
        m_callout_version  = std::move(version);
        m_callout_key      = 0;
    }
    // Runs at the start of each live frame with the camera set up and the context current (the
    // callouts' ID passes).
    void set_before_frame(std::function<void()> fn) { m_before_frame = std::move(fn); }
    // The Showcase canvas' mouse events before the camera gets them; the handler returns true for
    // the ones it used (dragging a callout).
    void set_mouse_handler(std::function<bool(wxMouseEvent &)> fn) { m_mouse_handler = std::move(fn); }
    bool on_mouse(wxMouseEvent &evt) { return m_mouse_handler && m_mouse_handler(evt); }

    // The live camera's view and projection cropped to the safe frame: world -> clip coordinates of
    // exactly what an export shows. False before the first live frame.
    bool frame_view_projection(Matrix4d &out) const;
    // S3: the ID pass, `long_side` pixels along the safe frame's longer side. The context must be
    // current (from the before-frame hook).
    bool render_id_pass(int long_side, ShowcaseIdPass &out);

    // The live frame (GLCanvas3D::render calls it for the Showcase canvas).
    void render_frame();

    // Renders the scene's export settings into `out`. `report` gets one line per decision taken
    // (supersampling, multisampling, fallbacks) for the log. False when nothing could be rendered.
    bool render_export(ShowcaseImage &out, std::string &report);

    // S4: a video renders many exports of one size in a row. Between these calls the off-screen
    // buffers stay allocated (sized for the first frame) instead of being made for each frame.
    void begin_batch();
    void end_batch();
    // S4: the live view's viewport and a projection fitted to the scene, for the camera as it is now
    // (a video frame's pose): what frame_view_projection() and the ID pass then use.
    void prepare_live_camera();

    // The safe frame of the last live frame, in canvas pixels, origin top-left.
    Showcase::RectI live_frame() const { return m_live_frame; }

    // S5, --showcase-render: no live frame is ever drawn in the hidden instance, so the "live view" is
    // a view exactly the export's size (the safe frame is all of it): the camera's zoom is then pixels
    // per mm of the output, and the callouts, the ID pass and the export all see the same frame.
    void set_offscreen_view(int width, int height);

    // S5: the layer build-up. `fraction` < 0 = none (the whole print), else 0..1 of it. With
    // toolpaths (Auto, when the plate is sliced) the slice's layers grow; otherwise the model is cut
    // by a plane (the clip fallback).
    // S5b: `infill` / `supports`: whether a toolpath build-up lays them.
    void set_build(double fraction, Showcase::BuildSource source, bool infill = true, bool supports = true);
    // S5c: the height (world, mm) a build-up at `fraction` has reached: the slice's layers or the clip's
    // plane. 1e30 when nothing is building (fraction < 0) or it is finished.
    double build_height_at(double fraction, Showcase::BuildSource source);
    // Only the current plate's objects (and its prime tower unless hidden) are active.
    void   update_visibility();
    // What the last frame used for the build-up: "" none, "toolpaths" or "clip".
    std::string build_used() const { return m_build_used; }
    // S5: the plate's slice can be drawn (Preview has its toolpaths loaded).
    static bool toolpaths_available();
    // S5: Preview gets its own toolpath settings back (the tab is left).
    static void release_toolpaths();
    // S5: the layer tops the build-up steps through without a slice (mm, the plate's objects).
    std::vector<double> model_layer_tops();

    // The objects of the current plate (and its prime tower when shown).
    BoundingBoxf3 objects_box() const;
    // Points the camera at the plate's objects from the front-right and fits them in the safe frame.
    void place_default_camera();
    // Fits the plate's objects in the safe frame, keeping the direction of view.
    void frame_model();

    // False on a legacy (GL 2.x) context or without framebuffer objects: the tab says so.
    static bool is_supported();

    // Frees the GL objects. The canvas' context must be current.
    void release_gl();

private:
    struct DrawTarget
    {
        int      width  = 0;
        int      height = 0;
        bool     live   = true;
        bool     run_ssao = true;
        float    ssao_radius_scale = 1.f;
        unsigned shadow_map_size   = 2048;
        // 1a: the framebuffer the scene is drawn into, and the objects' HDR layer that shares its
        // depth (0 = none: the objects tone-map in their shader and go straight into the scene).
        unsigned scene_fbo = 0;
        unsigned hdr_fbo   = 0;
        // The safe frame in [0,1] GL coordinates (origin bottom-left): x0, y0, x1, y1.
        float    frame[4] = { 0.f, 0.f, 1.f, 1.f };
    };

    bool            floor_drawn() const;
    void            floor_disc(Vec3d &center, double &radius) const;
    BoundingBoxf3   scene_box() const;
    RealisticRenderSettings render_settings(const DrawTarget &t) const;

    void            draw_scene(const DrawTarget &t);
    void            draw_background(const DrawTarget &t);
    void            draw_floor(const DrawTarget &t);
    void            draw_objects();
    // S5: the plate's toolpaths through Preview's viewer (the toolpath source, the build-up).
    bool            draw_toolpaths();
    bool            use_toolpaths() const;
    // S2: the objects through showcase_material; false when that shader is not there. 1a: `hdr`
    // writes linear light for the post pass instead of tone-mapped sRGB.
    bool            draw_objects_material(bool mirrored, bool hdr = false);
    // 1a: whether this frame can use the HDR layer and the post pass (the shaders are there, and
    // EDGESLICER_SHOWCASE_NO_HDR is not set).
    bool            want_hdr() const;
    // 1a: tone-maps the resolved HDR layer `hdr_texture` over the bound scene framebuffer.
    void            draw_post(unsigned int hdr_texture);
    // 1a: the live view's finished frame from its off-screen buffer onto the window.
    void            present(unsigned int color_texture);
    void            draw_overlay(int vp_w, int vp_h);
    void            ensure_fullscreen_quad();
    void            ensure_floor_model();
    bool            ensure_background_texture();
    void            ensure_watermark_texture(int frame_w, int frame_h);
    void            ensure_callout_texture(int frame_w, int frame_h);

    // S2 helpers.
    struct LayerInfo
    {
        int          mode   = 0;      // 0 none, 1 uniform, 2 table
        float        first  = 0.f;
        float        height = 0.f;
        unsigned int texture = 0;     // mode 2: R32F tops
        int          count  = 0;
        int          width  = 0;
    };
    const LayerInfo *layers_for(int object_idx);
    void            release_layers();
    // The scene's environment as a cube map; null when there is none (no manifest, no file).
    const Showcase::EnvironmentInfo *ensure_environment();
    bool            ensure_floor_texture(float &tile_mm, float &roughness);
    // The lights the material and the floor use (D14, or the editing headlight when not fixed).
    std::vector<Showcase::LightSource> scene_lights(const Camera &camera) const;
    // The blurred footprint under the objects (design 2.3); m_contact_valid tells if it was drawn.
    void            render_contact_shadow();
    // The objects mirrored in the floor, at half the viewport's size; m_refl_valid tells.
    void            render_reflection();
    // Frees the contact shadow and mirror buffers (an export may have sized them large).
    void            release_offscreen();

    GLCanvas3D                &m_canvas;
    const Showcase::Scene     *m_scene   = nullptr;
    const Showcase::Project   *m_project = nullptr;
    Showcase::RectI            m_live_frame;
    int                        m_live_vp_w = 0;
    int                        m_live_vp_h = 0;

    GLModel                   *m_floor_model = nullptr;   // a unit disc, owned

    unsigned int               m_bg_texture = 0;
    int                        m_bg_image_w = 0;
    int                        m_bg_image_h = 0;
    std::string                m_bg_image_key;

    // S2
    std::map<int, LayerInfo>   m_layers;
    bool                       m_layers_valid = false;
    unsigned int               m_env_texture = 0;
    std::string                m_env_key;
    unsigned int               m_floor_texture = 0;
    std::string                m_floor_key;
    float                      m_floor_tile_mm = 500.f;
    float                      m_floor_roughness = 0.6f;
    unsigned int               m_contact_fbo = 0;
    unsigned int               m_contact_tex[2] = { 0, 0 };
    bool                       m_contact_valid = false;
    float                      m_contact_rect[4] = { 0.f, 0.f, 1.f, 1.f };
    unsigned int               m_refl_fbo = 0;
    unsigned int               m_refl_tex = 0;
    unsigned int               m_refl_depth = 0;
    int                        m_refl_w = 0;
    int                        m_refl_h = 0;
    bool                       m_refl_valid = false;

    WatermarkProvider          m_watermark_provider;
    unsigned int               m_watermark_version = 1;
    unsigned int               m_wm_texture = 0;
    unsigned int               m_wm_texture_version = 0;
    int                        m_wm_frame_w = 0;
    int                        m_wm_frame_h = 0;
    Showcase::RectI            m_wm_rect;

    // S5
    double                     m_build_fraction = -1.;
    Showcase::BuildSource      m_build_source   = Showcase::BuildSource::Auto;
    bool                       m_build_infill   = true;   // S5b
    bool                       m_build_supports = true;   // S5b
    double                     m_build_clip_z   = 1e30;   // the clip fallback's plane, this frame
    std::string                m_build_used;
    std::vector<double>        m_model_tops;
    bool                       m_model_tops_valid = false;

    // S4
    bool                       m_batch = false;
    std::unique_ptr<ShowcaseFramebuffers> m_batch_fb;
    // 1a: the live view's off-screen buffers (the window's size and sample count), and the size
    // they could not be made at (not retried every frame).
    std::unique_ptr<ShowcaseFramebuffers> m_live_fb;
    int                        m_live_fb_failed_w = 0, m_live_fb_failed_h = 0;
    // 1a: the last draw_scene() put objects into the HDR layer (the post pass has work).
    bool                       m_hdr_drawn = false;
    int                        m_batch_w = 0, m_batch_h = 0, m_batch_msaa = -1;

    // S3
    OverlayProvider            m_callout_provider;
    std::function<uint64_t()>  m_callout_version;
    uint64_t                   m_callout_key = 0;
    unsigned int               m_callout_texture = 0;
    bool                       m_callout_shown = false;
    std::function<void()>      m_before_frame;
    std::function<bool(wxMouseEvent &)> m_mouse_handler;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseRenderer_hpp_
