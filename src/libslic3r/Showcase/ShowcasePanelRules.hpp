#pragma once
// Showcase panel: which rows apply to the scene as it is now. The panel hides the rest (hidden rows keep
// their stored values). Pure and header-only, so the rules are unit tested without a window.
//
// Environment: the scene stores ONE environment (lighting.environment). It lights and reflects on
// the model, and Background > Type = Environment also draws it behind the model. The panel shows that one
// value in the Background section when the backdrop is the environment, and in the Light section
// ("Lighting environment") for every other backdrop. No scene key was added.

#include "ShowcaseScene.hpp"

namespace Slic3r { namespace Showcase {

struct PanelVisibility
{
    // Background
    bool bg_environment = false;   // the environment picker
    bool bg_colour1     = false;
    bool bg_colour2     = false;
    bool bg_image       = false;
    bool bg_fit         = false;
    bool bg_blur        = false;
    // Floor
    bool floor_colour      = false;
    bool floor_shadow      = false;
    bool floor_grid        = false;
    bool floor_photo       = false;
    bool floor_picture     = false;
    bool floor_orientation = false;   // rotate, flip, tile size
    bool floor_reflection  = false;
    bool floor_contact     = false;
    // Light
    bool light_direction   = false;   // direction and height of the fixed key light
    bool light_shadow_dark = false;
    bool light_environment = false;   // "Lighting environment": the backdrop is not the environment
    bool light_env_levels  = false;   // environment brightness and rotation
    bool light_sun         = false;   // the HDRI's sun as the key light
    // Materials
    bool mat_layer_lines  = false;
    bool mat_toolpath     = false;    // smoothing
    bool mat_light_left   = false;
    // Callouts
    bool co_detail      = false;      // everything but the on/off box
    bool co_plain_colours = false;    // text and line colour
    bool co_box         = false;      // label background and opacity
    bool co_separator   = false;
    // Video
    bool vid_co_position   = false;
    bool vid_co_appearance = false;
    bool vid_build_parts   = false;
    // Watermark
    bool wm_detail = false;
};

// env_is_hdri: the scene's environment id names one of the bundled HDRIs (which have a sun).
inline PanelVisibility panel_visibility(const Scene &s, bool env_is_hdri)
{
    PanelVisibility v;
    const BackgroundType bt = s.background.type;
    const bool gradient = bt == BackgroundType::VerticalGradient || bt == BackgroundType::RadialGradient;
    const bool env_bg   = bt == BackgroundType::Environment;
    const bool env_set  = !s.lighting.environment.empty();
    // An Environment backdrop with no environment chosen falls back to the two colours as a gradient.
    const bool env_fallback = env_bg && !env_set;

    v.bg_environment = env_bg;
    v.bg_colour1     = bt == BackgroundType::Solid || gradient || env_fallback ||
                   (bt == BackgroundType::Image && s.background.fit == ImageFit::Fit);   // the bars around a fitted picture
    v.bg_colour2     = gradient || env_fallback;
    v.bg_image       = bt == BackgroundType::Image;
    v.bg_fit         = bt == BackgroundType::Image;
    v.bg_blur        = bt == BackgroundType::Image || (env_bg && env_set);

    const FloorType ft = s.floor.type;
    const bool      on = ft != FloorType::None;
    v.floor_colour      = ft == FloorType::Solid || ft == FloorType::Grid;
    v.floor_shadow      = on;
    v.floor_grid        = ft == FloorType::Grid;
    v.floor_photo       = ft == FloorType::Texture;
    v.floor_picture     = ft == FloorType::Image;
    v.floor_orientation = ft == FloorType::Texture || ft == FloorType::Image;
    v.floor_reflection  = on;
    v.floor_contact     = on;

    v.light_direction   = s.lighting.world_fixed;
    v.light_shadow_dark = s.lighting.shadows;
    v.light_environment = !env_bg;
    v.light_env_levels  = env_set;
    v.light_sun         = env_set && env_is_hdri;

    v.mat_layer_lines = s.source != "toolpaths";   // the toolpaths view already shows the real layers
    v.mat_toolpath    = s.source == "toolpaths";
    v.mat_light_left  = s.materials.lights_off;

    const bool boxed = s.callouts.style.shape == CalloutShape::Card || s.callouts.style.shape == CalloutShape::Pill;
    v.co_detail        = s.callouts.enabled;
    v.co_plain_colours = s.callouts.enabled && !s.callouts.style.auto_colors;
    v.co_box           = s.callouts.enabled && boxed;
    v.co_separator     = s.callouts.enabled && s.callouts.fields.color_name;

    v.vid_co_position   = s.callouts.enabled && s.video.callouts;
    v.vid_co_appearance = s.callouts.enabled && s.video.callouts && s.video.build.enabled;
    v.vid_build_parts   = s.video.build.enabled;

    v.wm_detail = s.export_settings.watermark.enabled;
    return v;
}

}} // namespace Slic3r::Showcase
