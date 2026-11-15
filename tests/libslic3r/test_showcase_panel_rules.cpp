// Showcase panel cleanup: which rows apply to the scene as it is (ShowcasePanelRules.hpp), and that
// the one environment value needs no new scene key.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcasePanelRules.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

using namespace Slic3r::Showcase;

TEST_CASE("Showcase panel: the environment shows where it is used", "[showcase][panel]")
{
    Scene s;
    s.lighting.environment = "studio";

    s.background.type = BackgroundType::Solid;
    PanelVisibility v = panel_visibility(s, false);
    CHECK(!v.bg_environment);
    CHECK(v.light_environment);
    CHECK(v.light_env_levels);

    s.background.type = BackgroundType::Environment;
    v = panel_visibility(s, false);
    CHECK(v.bg_environment);
    CHECK(!v.light_environment);   // the Background picker is the one value
    CHECK(v.bg_blur);
    CHECK(!v.bg_colour1);
    CHECK(!v.bg_colour2);
    CHECK(!v.bg_image);
    CHECK(!v.bg_fit);
}

TEST_CASE("Showcase panel: an Environment backdrop with no environment is the two colours", "[showcase][panel]")
{
    Scene s;
    s.background.type      = BackgroundType::Environment;
    s.lighting.environment = "";
    const PanelVisibility v = panel_visibility(s, false);
    CHECK(v.bg_colour1);
    CHECK(v.bg_colour2);
    CHECK(!v.bg_blur);
    CHECK(!v.light_env_levels);
    CHECK(!v.light_sun);
}

TEST_CASE("Showcase panel: background rows by type", "[showcase][panel]")
{
    Scene s;
    s.lighting.environment = "studio";

    s.background.type = BackgroundType::VerticalGradient;
    PanelVisibility v = panel_visibility(s, false);
    CHECK(v.bg_colour1);
    CHECK(v.bg_colour2);
    CHECK(!v.bg_blur);
    CHECK(!v.bg_image);

    s.background.type = BackgroundType::Solid;
    v = panel_visibility(s, false);
    CHECK(v.bg_colour1);
    CHECK(!v.bg_colour2);

    s.background.type = BackgroundType::Image;
    s.background.fit  = ImageFit::Fill;
    v = panel_visibility(s, false);
    CHECK(v.bg_image);
    CHECK(v.bg_fit);
    CHECK(v.bg_blur);
    CHECK(!v.bg_colour1);
    s.background.fit = ImageFit::Fit;   // the bars around the picture
    CHECK(panel_visibility(s, false).bg_colour1);

    s.background.type = BackgroundType::Transparent;
    v = panel_visibility(s, false);
    CHECK(!v.bg_colour1);
    CHECK(!v.bg_colour2);
    CHECK(!v.bg_image);
    CHECK(!v.bg_blur);
}

TEST_CASE("Showcase panel: the sun needs an HDRI", "[showcase][panel]")
{
    Scene s;
    s.lighting.environment = "studio";
    CHECK(!panel_visibility(s, false).light_sun);
    s.lighting.environment = "some_hdri";
    CHECK(panel_visibility(s, true).light_sun);
    s.lighting.environment = "";
    CHECK(!panel_visibility(s, true).light_sun);
}

TEST_CASE("Showcase panel: floor, light and materials rows", "[showcase][panel]")
{
    Scene s;
    s.floor.type = FloorType::None;
    PanelVisibility v = panel_visibility(s, false);
    CHECK(!v.floor_shadow);
    CHECK(!v.floor_reflection);
    CHECK(!v.floor_colour);

    s.floor.type = FloorType::Grid;
    v = panel_visibility(s, false);
    CHECK(v.floor_grid);
    CHECK(v.floor_colour);
    CHECK(!v.floor_photo);
    CHECK(!v.floor_orientation);

    s.floor.type = FloorType::Texture;
    v = panel_visibility(s, false);
    CHECK(v.floor_photo);
    CHECK(v.floor_orientation);
    CHECK(!v.floor_picture);
    CHECK(!v.floor_colour);

    s.floor.type = FloorType::Image;
    CHECK(panel_visibility(s, false).floor_picture);

    s.lighting.world_fixed = false;
    s.lighting.shadows     = false;
    v = panel_visibility(s, false);
    CHECK(!v.light_direction);
    CHECK(!v.light_shadow_dark);
    s.lighting.world_fixed = true;
    s.lighting.shadows     = true;
    v = panel_visibility(s, false);
    CHECK(v.light_direction);
    CHECK(v.light_shadow_dark);

    s.source = "model";
    v = panel_visibility(s, false);
    CHECK(v.mat_layer_lines);
    CHECK(!v.mat_toolpath);
    s.source = "toolpaths";
    v = panel_visibility(s, false);
    CHECK(!v.mat_layer_lines);
    CHECK(v.mat_toolpath);

    s.materials.lights_off = false;
    CHECK(!panel_visibility(s, false).mat_light_left);
    s.materials.lights_off = true;
    CHECK(panel_visibility(s, false).mat_light_left);
}

TEST_CASE("Showcase panel: callout, video and watermark rows", "[showcase][panel]")
{
    Scene s;
    s.callouts.enabled = false;
    PanelVisibility v  = panel_visibility(s, false);
    CHECK(!v.co_detail);
    CHECK(!v.co_plain_colours);
    CHECK(!v.vid_co_position);

    s.callouts.enabled           = true;
    s.callouts.style.auto_colors = true;
    v                            = panel_visibility(s, false);
    CHECK(v.co_detail);
    CHECK(!v.co_plain_colours);
    s.callouts.style.auto_colors = false;
    CHECK(panel_visibility(s, false).co_plain_colours);

    s.video.callouts      = true;
    s.video.build.enabled = false;
    v                     = panel_visibility(s, false);
    CHECK(v.vid_co_position);
    CHECK(!v.vid_co_appearance);
    CHECK(!v.vid_build_parts);
    s.video.build.enabled = true;
    v                     = panel_visibility(s, false);
    CHECK(v.vid_co_appearance);
    CHECK(v.vid_build_parts);

    s.export_settings.watermark.enabled = false;
    CHECK(!panel_visibility(s, false).wm_detail);
    s.export_settings.watermark.enabled = true;
    CHECK(panel_visibility(s, false).wm_detail);
}

TEST_CASE("Showcase panel: the rules read the scene only, so values are never changed", "[showcase][panel]")
{
    Scene a;
    a.lighting.environment = "studio";
    a.background.type      = BackgroundType::Environment;
    const Scene b          = a;
    (void) panel_visibility(a, true);
    CHECK(a == b);
}
