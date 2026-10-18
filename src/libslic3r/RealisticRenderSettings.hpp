#ifndef slic3r_RealisticRenderSettings_hpp_
#define slic3r_RealisticRenderSettings_hpp_

// The realistic view's switches (Orca #13704 / #14702 family) as one value, so a canvas can be
// driven by something other than the user's preferences. Prepare and Preview read them from the
// app config exactly as before (from_app_config); the Showcase tab hands its canvas its own copy,
// so a presentation render never touches, and is never changed by, the viewport preferences.

#include <array>

namespace Slic3r {

class AppConfig;

struct RealisticRenderSettings
{
    // Master switch ("Realistic view"). Everything below applies only while it is on.
    bool realistic = false;
    // Phong (per-pixel) shading; off = gouraud.
    bool phong = true;
    // The light-space shadow map: object-on-object, self and plate shadows.
    bool shadows = false;
    // Screen-space ambient occlusion.
    bool ssao = false;

    // --- set by the Showcase tab only; the preferences never change these ------------------------

    // The key light is fixed in the world (D14) instead of following the camera. When set,
    // world_light_dir is the direction TO the light in world space.
    bool                  world_light = false;
    std::array<double, 3> world_light_dir{ { -0.5, -0.5, 0.707 } };
    // Darkening of a fully shadowed object surface.
    float                 shadow_intensity = 0.35f;
    // Depth map size (the editing views use 2048).
    unsigned int          shadow_map_size = 2048;
    // SSAO kernel scale: 1 on screen, larger for supersampled exports (design 2.1).
    float                 ssao_radius_scale = 1.f;
    // SSAO keeps the alpha of what is under it (a transparent export) instead of writing 1.
    bool                  ssao_keep_alpha = false;
    // Cast the shadow map onto the printer's plate (the editing views). The Showcase floor does its
    // own receiving.
    bool                  plate_shadow = true;
    // Shadow casters are fitted to this box instead of the objects + plate (the Showcase floor
    // disc). Unused while min > max.
    std::array<double, 3> fit_min{ { 1., 1., 1. } };
    std::array<double, 3> fit_max{ { 0., 0., 0. } };

    bool has_fit_box() const { return fit_min[0] <= fit_max[0] && fit_min[1] <= fit_max[1] && fit_min[2] <= fit_max[2]; }

    // The preferences, as GLCanvas3D read them before this struct existed.
    static RealisticRenderSettings from_app_config(const AppConfig &config);
};

} // namespace Slic3r

#endif // slic3r_RealisticRenderSettings_hpp_
