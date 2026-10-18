#ifndef slic3r_GUI_ShowcaseRenderer_hpp_
#define slic3r_GUI_ShowcaseRenderer_hpp_

// Showcase tab (phase S1): draws the Showcase canvas and renders still images from it.
//
// One drawing function serves the live view and the export (design 2.2): backdrop, the printer's
// plate or a floor, the shadow map with a world-fixed light (D14), the objects of the current
// plate through the realistic view's Phong shader, and SSAO. The export draws the same frame into
// an off-screen framebuffer at up to 8192 px a side (D11), supersampled and multisampled, with the
// SSAO kernel scaled to the export size, and reads it back.
//
// A friend of GLCanvas3D: it drives that canvas's realistic-view passes (shadows, SSAO) through
// GLCanvas3D::set_render_settings_override(), never through the user's preferences.

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/RealisticRenderSettings.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

class GLCanvas3D;
class GLModel;

// An RGBA image, top row first, straight (not premultiplied) alpha.
struct ShowcaseImage
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> rgba;
    bool empty() const { return width <= 0 || height <= 0 || rgba.empty(); }
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
    // The backdrop image changed: upload it again on the next frame.
    void invalidate_images() { m_bg_image_key.clear(); }

    // The watermark preview over the live view. Called with the safe frame's size; returns the mark
    // for an image of that size, or an empty image. `version` changes whenever the mark does.
    using WatermarkProvider = std::function<bool(int frame_w, int frame_h, ShowcaseImage &mark, Showcase::RectI &where)>;
    void set_watermark_provider(WatermarkProvider provider) { m_watermark_provider = std::move(provider); }
    void invalidate_watermark() { ++m_watermark_version; }

    // The live frame (GLCanvas3D::render calls it for the Showcase canvas).
    void render_frame();

    // Renders the scene's export settings into `out`. `report` gets one line per decision taken
    // (supersampling, multisampling, fallbacks) for the log. False when nothing could be rendered.
    bool render_export(ShowcaseImage &out, std::string &report);

    // The safe frame of the last live frame, in canvas pixels, origin top-left.
    Showcase::RectI live_frame() const { return m_live_frame; }

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
        // The safe frame in [0,1] GL coordinates (origin bottom-left): x0, y0, x1, y1.
        float    frame[4] = { 0.f, 0.f, 1.f, 1.f };
    };

    void            update_visibility();
    bool            floor_drawn() const;
    void            floor_disc(Vec3d &center, double &radius) const;
    BoundingBoxf3   scene_box() const;
    RealisticRenderSettings render_settings(const DrawTarget &t) const;

    void            draw_scene(const DrawTarget &t);
    void            draw_background(const DrawTarget &t);
    void            draw_floor(const DrawTarget &t);
    void            draw_objects();
    void            draw_overlay(int vp_w, int vp_h);
    void            ensure_fullscreen_quad();
    void            ensure_floor_model();
    bool            ensure_background_texture();
    void            ensure_watermark_texture(int frame_w, int frame_h);

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

    WatermarkProvider          m_watermark_provider;
    unsigned int               m_watermark_version = 1;
    unsigned int               m_wm_texture = 0;
    unsigned int               m_wm_texture_version = 0;
    int                        m_wm_frame_w = 0;
    int                        m_wm_frame_h = 0;
    Showcase::RectI            m_wm_rect;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_ShowcaseRenderer_hpp_
