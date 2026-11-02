// Showcase realism 1h-2: the CPU side of the ground-projected environment, the backdrop tilt, the
// photo backplate's camera match and the depth of field (ShowcaseLens.hpp), and the scene keys.

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseHdr.hpp"
#include "libslic3r/Showcase/ShowcaseLens.hpp"
#include "libslic3r/Showcase/ShowcasePanelRules.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

using namespace Slic3r::Showcase;

namespace {
constexpr double LT_PI = 3.14159265358979323846;
double  len(const LensVec &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
LensVec unit(const LensVec &v)
{
    const double l = len(v);
    return { v[0] / l, v[1] / l, v[2] / l };
}
} // namespace

TEST_CASE("Showcase lens: ground projection and backplate make the floor a shadow catcher", "[showcase][lens]")
{
    Scene s;
    s.lighting.environment = "hdri_workshop";
    s.floor.type           = FloorType::Texture;
    s.background.type      = BackgroundType::Environment;
    CHECK_FALSE(ground_projection_active(s));
    CHECK(effective_floor_type(s) == FloorType::Texture);

    s.background.ground = true;
    CHECK(ground_projection_active(s));
    CHECK(effective_floor_type(s) == FloorType::ShadowCatcher);
    s.floor.type = FloorType::None;   // None stays None: no shadow at all
    CHECK(effective_floor_type(s) == FloorType::None);

    // No environment chosen: the backdrop falls back to its colours, the floor is the user's.
    s.floor.type           = FloorType::Grid;
    s.lighting.environment = "";
    CHECK_FALSE(ground_projection_active(s));
    CHECK(effective_floor_type(s) == FloorType::Grid);

    s.background.type = BackgroundType::Backplate;
    CHECK(effective_floor_type(s) == FloorType::ShadowCatcher);
}

TEST_CASE("Showcase lens: the ground dome's size", "[showcase][lens]")
{
    // Automatic height: three times the model's extent, kept to 60 .. 5000 mm.
    CHECK(ground_auto_height(60., 30., 48.) == Approx(3. * 48.));
    CHECK(ground_auto_height(200., 200., 10.) == Approx(3. * 0.5 * std::sqrt(2.) * 200.));
    CHECK(ground_auto_height(5., 5., 5.) == Approx(60.));
    CHECK(ground_auto_height(1e5, 1e5, 1e5) == Approx(5000.));

    Background bg;
    const LensVec mn{ { -30., -15., 0. } }, mx{ { 30., 15., 48. } };
    GroundDome d = ground_dome(bg, 1500., 8000., mn, mx);
    CHECK(d.height == Approx(144.));
    CHECK(d.radius == Approx(144. * 8000. / 1500.));
    CHECK(d.center[0] == Approx(0.));
    CHECK(d.center[1] == Approx(0.));
    CHECK(d.center[2] == Approx(144.));
    CHECK(d.floor_radius() == Approx(std::sqrt(d.radius * d.radius - 144. * 144.)));

    // Set values, then the world scale on both.
    bg.ground_height = 200.f;
    bg.ground_radius = 3000.f;
    bg.ground_scale  = 0.5f;
    d                = ground_dome(bg, 1500., 8000., mn, mx);
    CHECK(d.height == Approx(100.));
    CHECK(d.radius == Approx(1500.));
    // The dome is never so small that the floor disc vanishes.
    bg.ground_radius = 10.f;
    d                = ground_dome(bg, 1500., 8000., mn, mx);
    CHECK(d.radius == Approx(1.25 * d.height));
    CHECK(d.floor_radius() > 0.);
}

TEST_CASE("Showcase lens: ground lookup sees the floor, the dome and the sky", "[showcase][lens]")
{
    GroundDome d;
    d.height = 100.;
    d.radius = 1000.;
    d.center = { 0., 0., 100. };
    const LensVec eye{ { 0., -300., 150. } };

    // Straight down onto the floor next to the model: the panorama's floor, below its camera.
    double  t   = 0.;
    LensVec dir = unit({ 0., 0.5, -1. });
    LensVec l   = ground_lookup(d, eye, dir, &t);
    CHECK(t == Approx(150. / std::abs(dir[2])));
    const LensVec hit{ { eye[0] + t * dir[0], eye[1] + t * dir[1], eye[2] + t * dir[2] } };
    CHECK(hit[2] == Approx(0.).margin(1e-9));
    CHECK(l[0] == Approx(hit[0] - d.center[0]));
    CHECK(l[1] == Approx(hit[1] - d.center[1]));
    CHECK(l[2] == Approx(-100.));
    // From the panorama's own camera the lookup is the view direction itself.
    double t2 = 0.;
    LensVec l2 = ground_lookup(d, d.center, dir, &t2);
    CHECK(unit(l2)[0] == Approx(dir[0]).margin(1e-9));
    CHECK(unit(l2)[1] == Approx(dir[1]).margin(1e-9));
    CHECK(unit(l2)[2] == Approx(dir[2]).margin(1e-9));
    // Level: the dome's wall, `radius` from the panorama's camera.
    l = ground_lookup(d, eye, unit({ 0., 1., 0. }), &t);
    CHECK(len(l) == Approx(1000.));
    CHECK(t > 0.);
    // The floor and the dome meet at the floor disc's rim: the lookup is continuous there.
    const double  fr = d.floor_radius();
    const LensVec e2{ { 0., 0., 50. } };
    const LensVec in  = unit({ fr - 1e-6, 0., -50. });
    const LensVec out = unit({ fr + 1e-6, 0., -50. });
    const LensVec a = unit(ground_lookup(d, e2, in)), b = unit(ground_lookup(d, e2, out));
    CHECK(a[0] == Approx(b[0]).margin(1e-6));
    CHECK(a[2] == Approx(b[2]).margin(1e-6));
    // A camera far outside the dome looking past it: the panorama as if infinitely far.
    const LensVec far_eye{ { 0., -5000., 3000. } };
    const LensVec up = unit({ 0., 1., 1. });
    l = ground_lookup(d, far_eye, up, &t);
    CHECK(t == std::numeric_limits<double>::infinity());
    CHECK(l[1] == Approx(up[1]));
}

TEST_CASE("Showcase lens: the backdrop tilt turns the view up about the camera's right", "[showcase][lens]")
{
    const LensVec fwd = unit({ 0., 1., -0.4 });
    const auto    id  = backdrop_pitch_matrix(0., fwd);
    for (int i = 0; i < 9; ++i)
        CHECK(id[i] == (i % 4 == 0 ? 1. : 0.));
    const auto m = backdrop_pitch_matrix(20., fwd);
    // Forward tilts up by 20 degrees; the right axis (+X here) does not move.
    const LensVec r = apply_matrix(m, { 1., 0., 0. });
    CHECK(r[0] == Approx(1.));
    CHECK(r[1] == Approx(0.).margin(1e-12));
    const LensVec f = apply_matrix(m, fwd);
    CHECK(std::asin(f[2]) == Approx(std::asin(fwd[2]) + 20. * LT_PI / 180.));
    CHECK(len(f) == Approx(1.));
    // Straight down has no horizontal axis: no tilt.
    const auto down = backdrop_pitch_matrix(20., { 0., 0., -1. });
    CHECK(down[0] == 1.);
    CHECK(down[4] == 1.);
}

TEST_CASE("Showcase lens: focal length, horizon and pitch", "[showcase][lens]")
{
    // A 3:2 frame is the full frame: 50 mm gives the textbook 27 degrees vertically.
    CHECK(half_sensor_height_mm(1.5) == Approx(12.));
    CHECK(fov_y_from_focal(50., 1.5) * 180. / LT_PI == Approx(26.99).epsilon(1e-3));
    CHECK(focal_from_fov_y(fov_y_from_focal(35., 16. / 9.), 16. / 9.) == Approx(35.));
    // A level camera has its horizon in the middle; looking down puts it higher in the frame.
    const double fov = fov_y_from_focal(50., 16. / 9.);
    CHECK(pitch_for_horizon(0.5, fov) == Approx(0.).margin(1e-12));
    CHECK(pitch_for_horizon(0.8, fov) > 0.);
    CHECK(pitch_for_horizon(0.2, fov) < 0.);
    // The top edge of the frame is half the angle of view.
    CHECK(pitch_for_horizon(1.0, fov) == Approx(0.5 * fov));
    for (double h : { 0.1, 0.35, 0.5, 0.72, 0.95 })
        CHECK(horizon_for_pitch(pitch_for_horizon(h, fov), fov) == Approx(h));
    // The distance that gives the angle of view with the zoom unchanged.
    const double dist = distance_for_fov(540., 6., fov);
    CHECK(540. / (2. * 6. * dist) == Approx(std::tan(0.5 * fov)));
    CHECK(distance_for_fov(0., 6., fov) == 0.);

    // The matched eye looks at the target from that pitch, keeping the compass direction.
    const LensVec target{ { 10., 20., 15. } };
    const LensVec eye = matched_eye(target, unit({ 1., 1., -0.7 }), 0.3, 200.);
    const LensVec v   = unit({ target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] });
    CHECK(len({ target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] }) == Approx(200.));
    CHECK(std::asin(-v[2]) == Approx(0.3));
    CHECK(v[0] == Approx(v[1]));
}

TEST_CASE("Showcase lens: depth of field circle of confusion", "[showcase][lens]")
{
    DepthOfField dof;
    CHECK_FALSE(dof_setup(dof, 5., 16. / 9., 1080., 300.).active);   // off by default
    dof.enabled  = true;
    dof.f_number = 2.8f;
    // A 50 mm lens on a 16:9 frame 1080 px high, focused at 300 mm.
    const double fov = fov_y_from_focal(50., 16. / 9.);
    const double p11 = 1. / std::tan(0.5 * fov);
    DofSetup     s   = dof_setup(dof, p11, 16. / 9., 1080., 300.);
    REQUIRE(s.active);
    CHECK(s.focal_mm == Approx(50.));
    // Thin lens: aperture 50 / 2.8 mm; pixels per mm on the focus plane p11 * 540 / 300.
    CHECK(s.k_px == Approx((50. / 2.8) * p11 * 540. / 300.));
    CHECK(coc_px(s, 300.) == Approx(0.).margin(1e-12));   // in focus
    CHECK(s.max_px == Approx(DOF_MAX_COC * 1080.));
    CHECK(coc_px(s, 1e12) == Approx(std::min(s.k_px, s.max_px)));
    // In front and behind: the same thin-lens formula, nearer blurs faster.
    CHECK(coc_px(s, 330.) == Approx(std::min(s.k_px * (1. - 300. / 330.), s.max_px)));
    CHECK(coc_px(s, 270.) > coc_px(s, 330.));
    // Stopping down shrinks it in proportion.
    dof.f_number      = 5.6f;
    const DofSetup s2 = dof_setup(dof, p11, 16. / 9., 1080., 300.);
    CHECK(s2.k_px == Approx(s.k_px / 2.));
    // Twice the pixels, twice the circle (an export's supersampling sees the same picture).
    const DofSetup s3 = dof_setup(dof, p11, 16. / 9., 2160., 300.);
    CHECK(s3.k_px == Approx(2. * s2.k_px));
    CHECK(dof_taps(BokehQuality::Draft) < dof_taps(BokehQuality::Good));
    CHECK(dof_taps(BokehQuality::Good) < dof_taps(BokehQuality::Best));
}

TEST_CASE("Showcase lens: new scene keys default to the old look and round-trip", "[showcase][lens]")
{
    // An older scene: none of the keys, and nothing new written back.
    const std::string old_text = R"({ "schema": 2, "active": "plate1", "scenes": [ { "id": "plate1", "plate_index": 0,
        "background": { "type": "environment", "blur": 0.2 }, "lighting": { "environment": "hdri_loft" } } ] })";
    Project p;
    REQUIRE(scenes_from_json(old_text, p));
    REQUIRE(p.scenes.size() == 1);
    const Scene &s = p.scenes[0];
    CHECK(s.background.backdrop_pitch == 0.f);
    CHECK_FALSE(s.background.ground);
    CHECK(s.background.ground_height == 0.f);
    CHECK(s.background.ground_scale == 1.f);
    CHECK_FALSE(s.dof.enabled);
    const nlohmann::json out = nlohmann::json::parse(scenes_to_json(p));
    const nlohmann::json &bg = out["scenes"][0]["background"];
    for (const char *k : { "backdrop_pitch", "ground", "ground_height", "ground_radius", "ground_scale", "plate_horizon", "plate_focal" })
        CHECK_FALSE(bg.contains(k));
    CHECK_FALSE(out["scenes"][0].contains("dof"));

    // Set, written, read back.
    Project q = p;
    Scene  &t = q.scenes[0];
    t.background.type           = BackgroundType::Backplate;
    t.background.backdrop_pitch = 12.f;
    t.background.ground         = true;
    t.background.ground_height  = 180.f;
    t.background.ground_radius  = 2400.f;
    t.background.ground_scale   = 0.75f;
    t.background.plate_horizon  = 0.62f;
    t.background.plate_focal    = 26.f;
    t.dof.enabled               = true;
    t.dof.f_number              = 4.f;
    t.dof.focus_on_model        = false;
    t.dof.focus_mm              = 420.f;
    t.dof.quality               = BokehQuality::Best;
    Project back;
    REQUIRE(scenes_from_json(scenes_to_json(q), back));
    REQUIRE(back.scenes.size() == 1);
    CHECK(back.scenes[0] == t);
    CHECK(std::string(to_string(BackgroundType::Backplate)) == "backplate");

    // Turned back to the defaults: the keys go again (they are not kept from what was read).
    Scene u = back.scenes[0];
    u.background = Background();
    u.dof        = DepthOfField();
    Project r;
    r.scenes.push_back(u);
    const nlohmann::json out2 = nlohmann::json::parse(scenes_to_json(r));
    CHECK_FALSE(out2["scenes"][0]["background"].contains("ground"));
    CHECK_FALSE(out2["scenes"][0]["background"].contains("plate_focal"));
    CHECK_FALSE(out2["scenes"][0].contains("dof"));
}

TEST_CASE("Showcase lens: a backplate keeps its picture", "[showcase][lens]")
{
    Scene s;
    s.background.type  = BackgroundType::Backplate;
    s.background.image = "abc";
    const std::vector<std::string> used = s.used_images();
    CHECK(std::find(used.begin(), used.end(), "abc") != used.end());
}

TEST_CASE("Showcase lens: HDRI placement defaults in the manifest", "[showcase][lens]")
{
    HdriManifest m;
    REQUIRE(parse_hdri_manifest(R"({"hdris":[
        {"id":"a","file":"a.hdr","ground":{"height_mm":1500,"radius_mm":8000},"backdrop_pitch":12},
        {"id":"b","file":"b.hdr","ground":{"height_mm":-3,"radius_mm":"x"},"backdrop_pitch":500},
        {"id":"c","file":"c.hdr"}]})", m));
    REQUIRE(m.hdris.size() == 3);
    CHECK(m.find("a")->ground_height_mm == 1500.f);
    CHECK(m.find("a")->ground_radius_mm == 8000.f);
    CHECK(m.find("a")->backdrop_pitch == 12.f);
    // Out of range or not a number: the defaults.
    CHECK(m.find("b")->ground_height_mm == HdriInfo().ground_height_mm);
    CHECK(m.find("b")->ground_radius_mm == HdriInfo().ground_radius_mm);
    CHECK(m.find("b")->backdrop_pitch == 0.f);
    CHECK(m.find("c")->backdrop_pitch == 0.f);
}

TEST_CASE("Showcase panel: environment placement, backplate and depth of field rows", "[showcase][panel]")
{
    Scene s;
    s.lighting.environment = "hdri_workshop";
    s.background.type      = BackgroundType::Environment;
    s.floor.type           = FloorType::Grid;
    PanelVisibility v      = panel_visibility(s, true);
    CHECK(v.bg_ground);
    CHECK_FALSE(v.bg_ground_detail);
    CHECK(v.bg_pitch);
    CHECK_FALSE(v.bg_plate_match);
    CHECK(v.floor_grid);

    s.background.ground = true;
    v = panel_visibility(s, true);
    CHECK(v.bg_ground);
    CHECK(v.bg_ground_detail);
    CHECK_FALSE(v.bg_pitch);
    // The floor is the environment's: only the shadow catcher's rows apply.
    CHECK_FALSE(v.floor_grid);
    CHECK_FALSE(v.floor_colour);
    CHECK(v.floor_shadow);
    CHECK(v.floor_contact);

    // No environment: nothing to place.
    s.lighting.environment = "";
    v = panel_visibility(s, false);
    CHECK_FALSE(v.bg_ground);
    CHECK_FALSE(v.bg_ground_detail);
    CHECK_FALSE(v.bg_pitch);

    s.background.type = BackgroundType::Backplate;
    v = panel_visibility(s, false);
    CHECK(v.bg_image);
    CHECK_FALSE(v.bg_fit);
    CHECK(v.bg_blur);
    CHECK(v.bg_plate_match);
    CHECK_FALSE(v.bg_colour1);
    CHECK(v.light_environment);   // the photo is lit by the Light section's environment
    CHECK_FALSE(v.floor_grid);

    CHECK_FALSE(v.cam_dof_detail);
    s.dof.enabled = true;
    v = panel_visibility(s, false);
    CHECK(v.cam_dof_detail);
    CHECK_FALSE(v.cam_dof_focus);
    s.dof.focus_on_model = false;
    CHECK(panel_visibility(s, false).cam_dof_focus);
}

// ---- 1h-2b ------------------------------------------------------------------------------------

namespace {
// A minimal JPEG head: SOI, an APP1 Exif segment with one IFD0 entry (Orientation), then SOS.
std::vector<uint8_t> exif_jpeg(int orientation, bool little_endian)
{
    std::vector<uint8_t> tiff;
    auto u16 = [&tiff, little_endian](unsigned v) {
        if (little_endian) { tiff.push_back(uint8_t(v)); tiff.push_back(uint8_t(v >> 8)); }
        else { tiff.push_back(uint8_t(v >> 8)); tiff.push_back(uint8_t(v)); }
    };
    auto u32 = [&tiff, little_endian](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            tiff.push_back(uint8_t(little_endian ? v >> (8 * i) : v >> (8 * (3 - i))));
    };
    tiff.push_back(little_endian ? 'I' : 'M');
    tiff.push_back(little_endian ? 'I' : 'M');
    u16(42);
    u32(8);          // IFD0 right after the header
    u16(2);          // two entries: something else, then the orientation
    u16(0x010F); u16(2); u32(4); u32(0);       // Make (ASCII), value not used
    u16(0x0112); u16(3); u32(1); u16(unsigned(orientation)); u16(0);
    u32(0);          // no next IFD
    std::vector<uint8_t> j = { 0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x04, 0x00, 0x00 };   // SOI, a tiny APP0
    const size_t len = 2 + 6 + tiff.size();
    j.insert(j.end(), { 0xFF, 0xE1, uint8_t(len >> 8), uint8_t(len), 'E', 'x', 'i', 'f', 0, 0 });
    j.insert(j.end(), tiff.begin(), tiff.end());
    j.insert(j.end(), { 0xFF, 0xDA, 0x00, 0x02, 0xFF, 0xD9 });
    return j;
}

// A 3 x 2 image whose pixel (x, y) has red = x, green = y.
std::vector<uint8_t> coord_image(int w, int h)
{
    std::vector<uint8_t> rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            rgba.insert(rgba.end(), { uint8_t(x), uint8_t(y), 0, 255 });
    return rgba;
}
} // namespace

TEST_CASE("Showcase lens: EXIF orientation is read from a JPEG", "[showcase][lens]")
{
    for (int o = 1; o <= 8; ++o) {
        CHECK(exif_orientation(exif_jpeg(o, true).data(), exif_jpeg(o, true).size()) == o);
        CHECK(exif_orientation(exif_jpeg(o, false).data(), exif_jpeg(o, false).size()) == o);
    }
    // Out of range, not a JPEG, cut short: as stored.
    CHECK(exif_orientation(exif_jpeg(9, true).data(), exif_jpeg(9, true).size()) == 1);
    const std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    CHECK(exif_orientation(png.data(), png.size()) == 1);
    std::vector<uint8_t> cut = exif_jpeg(6, true);
    cut.resize(20);
    CHECK(exif_orientation(cut.data(), cut.size()) == 1);
    CHECK(exif_orientation(nullptr, 0) == 1);
}

TEST_CASE("Showcase lens: orientations turn the pixels the EXIF way", "[showcase][lens]")
{
    const int W = 3, H = 2;
    auto at = [](const std::vector<uint8_t> &img, int w, int x, int y) {
        const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 4;
        return std::make_pair(int(img[i]), int(img[i + 1]));   // the source (x, y) it came from
    };
    struct Expect { int o; int w; int h; int x; int y; int sx; int sy; };
    // The output's top-left pixel and where it comes from in the stored image.
    const Expect cases[] = {
        { 1, 3, 2, 0, 0, 0, 0 }, { 2, 3, 2, 0, 0, 2, 0 }, { 3, 3, 2, 0, 0, 2, 1 }, { 4, 3, 2, 0, 0, 0, 1 },
        { 5, 2, 3, 0, 0, 0, 0 }, { 6, 2, 3, 0, 0, 0, 1 }, { 7, 2, 3, 0, 0, 2, 1 }, { 8, 2, 3, 0, 0, 2, 0 },
        // Turned 90 clockwise: the stored top-left ends at the top right.
        { 6, 2, 3, 1, 0, 0, 0 },
        // Turned 90 counter-clockwise: the stored top-left ends at the bottom left.
        { 8, 2, 3, 0, 2, 0, 0 },
    };
    for (const Expect &e : cases) {
        std::vector<uint8_t> img = coord_image(W, H);
        int w = W, h = H;
        orient_rgba(img, w, h, e.o);
        INFO("orientation " << e.o);
        CHECK(w == e.w);
        CHECK(h == e.h);
        CHECK(at(img, w, e.x, e.y) == std::make_pair(e.sx, e.sy));
    }
    // The user's turn: counter-clockwise; 90 then 270 is the stored image again.
    CHECK(rotation_orientation(0) == 1);
    CHECK(rotation_orientation(90) == 8);
    CHECK(rotation_orientation(180) == 3);
    CHECK(rotation_orientation(270) == 6);
    std::vector<uint8_t> img = coord_image(W, H);
    int w = W, h = H;
    orient_rgba(img, w, h, rotation_orientation(90));
    orient_rgba(img, w, h, rotation_orientation(270));
    CHECK(img == coord_image(W, H));
    CHECK(w == W);
}

TEST_CASE("Showcase lens: backplate turn and toolpath switches are saved only when set", "[showcase][lens]")
{
    Project p;
    Scene  &s = p.ensure_scene_for_plate(0);
    nlohmann::json out = nlohmann::json::parse(scenes_to_json(p));
    for (const char *k : { "toolpath_supports", "toolpath_skirt", "toolpath_infill" })
        CHECK_FALSE(out["scenes"][0]["materials"].contains(k));
    CHECK_FALSE(out["scenes"][0]["background"].contains("plate_rotation"));
    CHECK_FALSE(out["scenes"][0]["background"].contains("plate_flip"));

    s.background.plate_rotation = 270;
    s.background.plate_flip     = true;
    s.materials.toolpath_supports = false;
    s.materials.toolpath_skirt    = false;
    s.materials.toolpath_infill   = false;
    Project back;
    REQUIRE(scenes_from_json(scenes_to_json(p), back));
    REQUIRE(back.scenes.size() == 1);
    CHECK(back.scenes[0] == s);

    // Back on: the keys go again.
    back.scenes[0].materials.toolpath_supports = true;
    back.scenes[0].background.plate_flip       = false;
    out = nlohmann::json::parse(scenes_to_json(back));
    CHECK_FALSE(out["scenes"][0]["materials"].contains("toolpath_supports"));
    CHECK(out["scenes"][0]["materials"].contains("toolpath_skirt"));
    CHECK_FALSE(out["scenes"][0]["background"].contains("plate_flip"));
}

TEST_CASE("Showcase panel: toolpath switches and the backplate's turn", "[showcase][panel]")
{
    Scene s;
    s.source = "model";
    CHECK_FALSE(panel_visibility(s, false).mat_toolpath);
    s.source = "toolpaths";
    CHECK(panel_visibility(s, false).mat_toolpath);   // Supports, Brim and skirt, Infill under Show
    s.background.type = BackgroundType::Backplate;
    CHECK(panel_visibility(s, false).bg_plate_match);   // Rotate photo, Flip, horizon, lens, Match camera
}
