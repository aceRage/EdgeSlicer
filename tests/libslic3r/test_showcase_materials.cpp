// Showcase tab (phase S2): finish inference (D4), material parameters, the light rig, the
// procedural layer-line maths, the S2 scene fields in scenes.json (and S1 files read by this
// build), the built-in presets against the bundled asset manifests, and the WebP encoder
// (src/libslic3r/Showcase/ShowcaseMaterial, ShowcaseAssets, ShowcaseScene, ShowcaseWebP).

#include <catch2/catch.hpp>

#include "libslic3r/Showcase/ShowcaseAssets.hpp"
#include "libslic3r/Showcase/ShowcaseMaterial.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/Showcase/ShowcaseWebP.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <sstream>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

std::string resources_path() { return std::string(TEST_DATA_DIR) + "/../../resources"; }

std::string read_text(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// ------------------------------------------------------------------------------------------------
// D4: the finish from the preset name and type
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: the filament finish is inferred from the preset name and type", "[Showcase][Materials]")
{
    struct Case { const char *name; const char *type; Finish expected; };
    const Case cases[] = {
        { "Generic PLA", "PLA", Finish::Standard },
        { "Snapmaker PLA Basic @U1", "PLA", Finish::Standard },
        { "Generic ABS", "ABS", Finish::Standard },
        { "Bambu PLA Matte @BBL X1C", "PLA", Finish::Matte },
        { "Polymaker PolyTerra PLA Matt", "PLA", Finish::Matte },
        { "Bambu PLA Aero", "PLA-AERO", Finish::Matte },
        { "Snapmaker PLA Silk @U1", "PLA", Finish::Silk },
        { "eSUN Silk+ PLA", "PLA", Finish::Silk },
        { "Sunlu Shiny PLA", "PLA", Finish::Silk },
        { "Generic PETG", "PETG", Finish::Gloss },
        { "Prusament PETG HF", "PETG", Finish::Gloss },
        { "Generic PCTG", "PCTG", Finish::Gloss },
        { "Overture High Gloss PLA", "PLA", Finish::Gloss },
        { "Bambu PETG Translucent", "PETG", Finish::Transparent },
        { "Clear PETG", "PETG", Finish::Transparent },
        { "Crystal PLA", "PLA", Finish::Transparent },
        { "Generic PETG-CF", "PETG-CF", Finish::CarbonFiber },
        { "Bambu PLA-CF", "PLA-CF", Finish::CarbonFiber },
        { "Polymaker PA6-GF", "PA6-GF", Finish::CarbonFiber },
        { "Something PA", "PA-CF10", Finish::CarbonFiber },
        { "Proto-pasta Copper Metal PLA", "PLA", Finish::Metallic },
        { "Bronzefill", "PLA", Finish::Standard },   // one word: not "bronze"
    };
    for (const Case &c : cases) {
        INFO(c.name << " / " << c.type);
        CHECK(infer_finish(c.name, c.type) == c.expected);
    }

    SECTION("whole words only") {
        // A vendor called MatterHackers is not matte; "Clearance" is not clear.
        CHECK(infer_finish("MatterHackers Build PLA", "PLA") == Finish::Standard);
        CHECK(infer_finish("Clearance PLA", "PLA") == Finish::Standard);
        CHECK(infer_finish("", "") == Finish::Standard);
    }
    SECTION("precedence: transparent over gloss, silk over metallic") {
        CHECK(infer_finish("PETG Translucent Blue", "PETG") == Finish::Transparent);
        CHECK(infer_finish("Silk Copper PLA", "PLA") == Finish::Silk);
        CHECK(infer_finish("Matte PETG", "PETG") == Finish::Matte);
    }
}

TEST_CASE("Showcase: finish names and material parameters", "[Showcase][Materials]")
{
    for (Finish f : all_finishes()) {
        Finish back;
        REQUIRE(from_string(to_string(f), back));
        CHECK(back == f);
        const MaterialParams p = material_params(f);
        CHECK(p.roughness > 0.f);
        CHECK(p.roughness <= 1.f);
        CHECK(p.f0 > 0.f);
        CHECK(p.alpha > 0.f);
        CHECK(p.alpha <= 1.f);
    }
    Finish bad = Finish::Gloss;
    CHECK_FALSE(from_string("velvet", bad));
    CHECK(bad == Finish::Gloss);

    const MaterialParams matte = material_params(Finish::Matte);
    const MaterialParams gloss = material_params(Finish::Gloss);
    const MaterialParams silk  = material_params(Finish::Silk);
    const MaterialParams clear = material_params(Finish::Transparent);
    CHECK(matte.roughness > gloss.roughness);
    CHECK(matte.line_shade > gloss.line_shade);   // matte hides layer lines less
    CHECK(silk.anisotropy > 0.5f);                 // the silk streak
    CHECK(silk.metal_tint > 0.f);                  // colour-tinted highlight
    CHECK(gloss.clearcoat > 0.f);
    CHECK(clear.alpha < 1.f);                      // D15: drawn translucent
    CHECK(material_params(Finish::Standard).alpha == 1.f);
}

TEST_CASE("Showcase: per-filament finish overrides", "[Showcase][Materials]")
{
    Materials m;
    CHECK(m.layer_lines == LayerLines::Subtle);   // R11
    CHECK(m.override_for(1) == nullptr);
    CHECK(m.finish_for(1, "Snapmaker PLA Silk", "PLA") == Finish::Silk);

    const Finish matte = Finish::Matte, gloss = Finish::Gloss;
    m.set_override(3, &matte);
    m.set_override(1, &gloss);
    REQUIRE(m.overrides.size() == 2);
    CHECK(m.overrides[0].filament == 1);   // kept sorted
    CHECK(m.finish_for(1, "Snapmaker PLA Silk", "PLA") == Finish::Gloss);
    CHECK(m.finish_for(3, "Generic PETG", "PETG") == Finish::Matte);
    m.set_override(1, &matte);
    CHECK(m.overrides.size() == 2);
    m.set_override(1, nullptr);
    CHECK(m.override_for(1) == nullptr);
    CHECK(m.overrides.size() == 1);
    m.set_override(0, &matte);   // slots are 1-based
    CHECK(m.overrides.size() == 1);
}

// ------------------------------------------------------------------------------------------------
// Light rig
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: colour temperature and the three-point lights", "[Showcase][Lighting]")
{
    const auto white = color_temperature_rgb(6500.f);
    CHECK_THAT(white[0], WithinAbs(1., 0.05));
    CHECK_THAT(white[1], WithinAbs(1., 0.05));
    CHECK_THAT(white[2], WithinAbs(1., 0.08));
    const auto warm = color_temperature_rgb(3000.f);
    CHECK(warm[0] > warm[1]);
    CHECK(warm[1] > warm[2]);
    const auto cool = color_temperature_rgb(10000.f);
    CHECK(cool[2] > cool[0]);
    CHECK(std::max({ cool[0], cool[1], cool[2] }) == 1.f);
    const auto clamped = color_temperature_rgb(-5.f);
    CHECK(clamped[0] == 1.f);

    Lighting li;
    std::vector<LightSource> lights = three_point_lights(li);
    REQUIRE(lights.size() == 3);   // key, fill, rim
    const auto key = light_direction(li.azimuth_deg, li.elevation_deg);
    CHECK_THAT(lights[0].dir[0], WithinAbs(key[0], 1e-5));
    CHECK_THAT(lights[0].dir[2], WithinAbs(key[2], 1e-5));
    for (const LightSource &l : lights)
        CHECK_THAT(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2], WithinAbs(1., 1e-5));
    // The rim is behind the subject as the key sees it: it points away from the key in plan.
    CHECK(lights[0].dir[0] * lights[2].dir[0] + lights[0].dir[1] * lights[2].dir[1] < 0.f);

    li.fill = false;
    li.rim_intensity = 0.f;
    lights = three_point_lights(li);
    CHECK(lights.size() == 1);
    li.key_intensity = 2.f;
    li.key_temperature = 6500.f;
    CHECK_THAT(three_point_lights(li)[0].color[0], WithinAbs(2., 0.1));
}

// ------------------------------------------------------------------------------------------------
// Layer lines (design 2.6.2)
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: layer lines fade out under about 1.5 px", "[Showcase][LayerLines]")
{
    CHECK(layer_line_fade(0.f) == 0.f);
    CHECK(layer_line_fade(1.f) == 0.f);
    CHECK_THAT(layer_line_fade(1.5f), WithinAbs(0.5, 1e-6));
    CHECK(layer_line_fade(2.f) == 1.f);
    CHECK(layer_line_fade(40.f) == 1.f);
    for (float px = 1.f; px < 2.f; px += 0.1f)
        CHECK(layer_line_fade(px + 0.1f) >= layer_line_fade(px));

    // A 0.2 mm layer where z changes by 0.4 mm a pixel is half a pixel: invisible. At 4K with 2x
    // supersampling the same shot gives 0.05 mm a pixel: four pixels, full strength.
    CHECK_THAT(layer_pixels(0.2f, 0.4f), WithinAbs(0.5, 1e-6));
    CHECK(layer_line_fade(layer_pixels(0.2f, 0.4f)) == 0.f);
    CHECK(layer_line_fade(layer_pixels(0.2f, 0.05f)) == 1.f);
    CHECK(layer_pixels(0.f, 0.1f) == 0.f);
    CHECK(layer_pixels(0.2f, 0.f) > 1e6f);   // a top surface: z does not change

    CHECK(layer_line_depth(LayerLines::Off) == 0.f);
    CHECK(layer_line_depth(LayerLines::Subtle) < layer_line_depth(LayerLines::Real));
    CHECK(layer_line_depth(LayerLines::Real) < layer_line_depth(LayerLines::Exaggerated));
    for (LayerLines v : { LayerLines::Off, LayerLines::Subtle, LayerLines::Real, LayerLines::Exaggerated }) {
        LayerLines back = LayerLines::Off;
        REQUIRE(from_string(to_string(v), back));
        CHECK(back == v);
    }
}

TEST_CASE("Showcase: where a point sits in its layer", "[Showcase][LayerLines]")
{
    SECTION("uniform layers after the first") {
        LayerSpan s = uniform_layer_at(0.1f, 0.3f, 0.2f);
        CHECK(s.bottom == 0.f);
        CHECK_THAT(s.top, WithinAbs(0.3, 1e-6));
        s = uniform_layer_at(0.45f, 0.3f, 0.2f);
        CHECK_THAT(s.bottom, WithinAbs(0.3, 1e-6));
        CHECK_THAT(s.top, WithinAbs(0.5, 1e-6));
        CHECK_THAT(layer_phase(s, 0.4f), WithinAbs(0.5, 1e-5));
        s = uniform_layer_at(10.05f, 0.3f, 0.2f);
        CHECK_THAT(s.height(), WithinAbs(0.2, 1e-5));
        CHECK(s.bottom <= 10.05f);
        CHECK(s.top >= 10.05f);
    }
    SECTION("a table from generate_object_layers") {
        // 0.3 first layer, then 0.2, then a variable stretch of 0.1 mm layers.
        const std::vector<double> boundaries = { 0., 0.3, 0.3, 0.5, 0.5, 0.7, 0.7, 0.8, 0.8, 0.9 };
        LayerTable t(boundaries);
        REQUIRE(t.size() == 5);
        CHECK_FALSE(t.uniform());
        LayerSpan s = t.layer_at(0.75f);
        CHECK_THAT(s.bottom, WithinAbs(0.7, 1e-6));
        CHECK_THAT(s.top, WithinAbs(0.8, 1e-6));
        s = t.layer_at(0.f);
        CHECK(s.bottom == 0.f);
        CHECK_THAT(s.top, WithinAbs(0.3, 1e-6));
        s = t.layer_at(-1.f);   // below the bed: the first layer
        CHECK_THAT(s.top, WithinAbs(0.3, 1e-6));
        s = t.layer_at(1.05f);  // past the top: the last layer continued
        CHECK_THAT(s.height(), WithinAbs(0.1, 1e-5));
        CHECK(s.bottom <= 1.05f);
        CHECK(s.top >= 1.05f);
    }
    SECTION("a uniform table needs no lookup") {
        std::vector<double> boundaries;
        double z = 0.;
        for (int i = 0; i < 50; ++i) {
            const double h = i == 0 ? 0.24 : 0.12;
            boundaries.push_back(z);
            boundaries.push_back(z + h);
            z += h;
        }
        LayerTable t(boundaries);
        CHECK(t.uniform());
        CHECK_THAT(t.first_layer_height(), WithinAbs(0.24, 1e-6));
        CHECK_THAT(t.layer_height(), WithinAbs(0.12, 1e-5));
        // Both lookups agree.
        for (float q = 0.013f; q < 5.f; q += 0.37f) {   // never on a boundary
            const LayerSpan a = t.layer_at(q);
            const LayerSpan b = uniform_layer_at(q, t.first_layer_height(), t.layer_height());
            CHECK_THAT(a.bottom, WithinAbs(b.bottom, 1e-4));
            CHECK_THAT(a.top, WithinAbs(b.top, 1e-4));
        }
    }
    SECTION("degenerate input") {
        LayerTable empty;
        CHECK(empty.empty());
        CHECK(empty.layer_at(1.f).height() == 0.f);
        CHECK(layer_phase(LayerSpan(), 1.f) == 0.f);
        CHECK(uniform_layer_at(1.f, 0.f, 0.f).height() == 0.f);
    }
}

// ------------------------------------------------------------------------------------------------
// Scenes: the S2 fields
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: the S2 scene fields survive JSON", "[Showcase]")
{
    Project p;
    Scene &s = p.ensure_scene_for_plate(0, PresetId::DeskWood);
    s.background.type = BackgroundType::Environment;
    s.background.blur = 0.25f;
    s.floor.type          = FloorType::Image;
    s.floor.image         = p.add_image({ 1, 2, 3, 4, 5 }, "jpg");
    s.floor.texture       = "marble";
    s.floor.texture_scale = 2.f;
    s.floor.texture_size  = 750.f;
    s.floor.reflection    = 0.3f;
    s.floor.contact_shadow = 0.9f;
    s.lighting.key_intensity    = 1.7f;
    s.lighting.key_temperature  = 3200.f;
    s.lighting.fill             = false;
    s.lighting.rim_intensity    = 1.2f;
    s.lighting.environment      = "sunset";
    s.lighting.env_intensity    = 1.4f;
    s.lighting.env_rotation_deg = 90.f;
    s.lighting.exposure         = -0.5f;
    s.lighting.shadow_softness  = 2.5f;
    s.materials.layer_lines     = LayerLines::Exaggerated;
    const Finish silk = Finish::Silk, clear = Finish::Transparent;
    s.materials.set_override(2, &silk);
    s.materials.set_override(4, &clear);
    s.export_settings.format       = ImageFormat::WebP;
    s.export_settings.use_as_cover = true;

    Project back;
    REQUIRE(scenes_from_json(scenes_to_json(p), back));
    REQUIRE(back.scenes.size() == 1);
    CHECK(back.scenes[0] == s);
    CHECK(back.schema == 2);
    CHECK(scenes_to_json(back) == scenes_to_json(p));
    // The floor picture is one of the scene's images, so the 3MF writes it.
    const std::vector<std::string> used = back.scenes[0].used_images();
    CHECK(std::find(used.begin(), used.end(), s.floor.image) != used.end());

    const json j = json::parse(scenes_to_json(p));
    CHECK(j["schema"] == SCHEMA_VERSION);
    CHECK(j["scenes"][0]["materials"]["layer_lines"] == "exaggerated");
    CHECK(j["scenes"][0]["materials"]["filaments"].size() == 2);
    CHECK(j["scenes"][0]["materials"]["filaments"][0]["slot"] == 2);
    CHECK(j["scenes"][0]["materials"]["filaments"][0]["finish"] == "silk");
    CHECK(j["scenes"][0]["export"]["format"] == "webp");
    CHECK(j["scenes"][0]["floor"]["type"] == "image");
    CHECK(j["scenes"][0]["background"]["type"] == "environment");
}

TEST_CASE("Showcase: an S1 scenes file still opens, with the S2 settings at their defaults", "[Showcase]")
{
    // Written by the S1 build: schema 1, none of the S2 keys.
    const std::string s1 = R"({"schema":1,"active":"plate1","scenes":[{"id":"plate1","name":"Plate 1","plate_index":0,
        "source":"model","preset_base":"dark_gradient",
        "background":{"type":"vertical_gradient","color1":"#3A3D42","color2":"#0B0C0E","image":"","fit":"fill"},
        "floor":{"type":"grid","color":"#1A1B1E","shadow_strength":0.6,"grid_spacing":10,"hide_plate":true,"hide_wipe_tower":true},
        "lighting":{"world_fixed":true,"azimuth":200,"elevation":40,"shadows":true,"shadow_intensity":0.45,"ssao":true},
        "views":[],"export":{"width":1920,"height":1080,"quality":"high","format":"png","jpeg_quality":92,
        "watermark":{"enabled":false}}}]})";
    Project p;
    REQUIRE(scenes_from_json(s1, p));
    CHECK_FALSE(p.read_only());   // older, not newer
    REQUIRE(p.scenes.size() == 1);
    const Scene &s = p.scenes[0];
    CHECK(s.floor.type == FloorType::Grid);
    CHECK(s.lighting.azimuth_deg == 200.f);
    CHECK(s.background.blur == 0.f);
    CHECK(s.floor.reflection == 0.f);
    CHECK(s.floor.contact_shadow == Floor().contact_shadow);
    CHECK(s.lighting.key_intensity == 1.f);
    CHECK(s.lighting.environment == Lighting().environment);
    CHECK(s.materials == Materials());
    CHECK_FALSE(s.export_settings.use_as_cover);   // D10: never on by itself
    CHECK(s.export_settings.format == ImageFormat::PNG);
    // Saved again it becomes schema 2 and keeps what S1 wrote.
    const json out = json::parse(scenes_to_json(p));
    CHECK(out["schema"] == 2);
    CHECK(out["scenes"][0]["floor"]["grid_spacing"] == 10);
    CHECK(out["scenes"][0]["preset_base"] == "dark_gradient");
}

TEST_CASE("Showcase: bad S2 values are clamped or ignored", "[Showcase]")
{
    Project p;
    REQUIRE(scenes_from_json(R"({"schema":2,"scenes":[{"id":"a","plate_index":0,
        "background":{"type":"environment","blur":9},
        "floor":{"type":"carpet","reflection":-3,"contact_shadow":"lots","texture_scale":1000},
        "lighting":{"key_intensity":50,"key_temperature":100,"exposure":12,"shadow_softness":0,"env_intensity":-1},
        "materials":{"layer_lines":"insane","filaments":[{"slot":0,"finish":"silk"},{"slot":2,"finish":"velvet"},{"slot":3,"finish":"matte"},"x"]},
        "export":{"format":"gif","use_as_cover":"yes"}}]})", p));
    REQUIRE(p.scenes.size() == 1);
    const Scene &s = p.scenes[0];
    CHECK(s.background.type == BackgroundType::Environment);
    CHECK(s.background.blur == 1.f);
    CHECK(s.floor.type == Floor().type);
    CHECK(s.floor.reflection == 0.f);
    CHECK(s.floor.contact_shadow == Floor().contact_shadow);
    CHECK(s.floor.texture_scale == 10.f);
    CHECK(s.lighting.key_intensity == 3.f);
    CHECK(s.lighting.key_temperature == 2000.f);
    CHECK(s.lighting.exposure == 3.f);
    CHECK(s.lighting.shadow_softness == 0.5f);
    CHECK(s.lighting.env_intensity == 0.f);
    CHECK(s.materials.layer_lines == LayerLines::Subtle);
    REQUIRE(s.materials.overrides.size() == 1);
    CHECK(s.materials.overrides[0].filament == 3);
    CHECK(s.materials.overrides[0].finish == Finish::Matte);
    CHECK(s.export_settings.format == ImageFormat::PNG);
    CHECK_FALSE(s.export_settings.use_as_cover);
}

TEST_CASE("Showcase: unknown keys inside materials are kept", "[Showcase]")
{
    Project p;
    REQUIRE(scenes_from_json(R"({"schema":2,"scenes":[{"id":"a","plate_index":0,
        "materials":{"layer_lines":"real","future_mat":{"k":1},"filaments":[]}}]})", p));
    REQUIRE(p.scenes.size() == 1);
    CHECK(p.scenes[0].materials.layer_lines == LayerLines::Real);
    const json out = json::parse(scenes_to_json(p));
    CHECK(out["scenes"][0]["materials"]["future_mat"]["k"] == 1);
}

// ------------------------------------------------------------------------------------------------
// Presets and the bundled assets
// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: the built-in presets load and name bundled assets", "[Showcase][Presets]")
{
    EnvironmentManifest envs;
    std::string         error;
    REQUIRE(parse_environment_manifest(read_text(resources_path() + "/" + ENVIRONMENTS_DIR + "/" + ENVIRONMENTS_MANIFEST), envs, &error));
    INFO(error);
    FloorManifest floors;
    REQUIRE(parse_floor_manifest(read_text(resources_path() + "/" + FLOORS_DIR + "/" + FLOORS_MANIFEST), floors, &error));

    const std::vector<PresetId> presets = builtin_presets();
    REQUIRE(presets.size() == 5);
    CHECK(presets.front() == PresetId::StudioWhite);   // the default (design 1.3)
    for (PresetId id : presets) {
        INFO(to_string(id));
        PresetId back;
        REQUIRE(from_string(to_string(id), back));
        CHECK(back == id);
        const Scene s = make_scene(id, 0);
        CHECK(s.preset_base == to_string(id));
        CHECK(s.lighting.world_fixed);   // D14
        if (!s.lighting.environment.empty())
            CHECK(envs.find(s.lighting.environment) != nullptr);
        if (s.floor.type == FloorType::Texture)
            CHECK(floors.find(s.floor.texture) != nullptr);
        CHECK(floors.find(Floor().texture) != nullptr);   // the floor a user switches to first
        CHECK_FALSE(s.export_settings.use_as_cover);      // D10
    }
    CHECK(make_scene(PresetId::DeskWood, 0).floor.type == FloorType::Texture);
    CHECK(make_scene(PresetId::Outdoor, 0).background.type == BackgroundType::Environment);
    CHECK_FALSE(make_scene(PresetId::DarkGradient, 0).lighting.fill);   // key + strong rim only

    SECTION("a preset keeps the materials, the camera, the export and the user's floor picture") {
        Scene s = make_scene(PresetId::StudioWhite, 0);
        const Finish silk = Finish::Silk;
        s.materials.set_override(1, &silk);
        s.floor.image = std::string(64, 'a');
        s.camera.valid = true;
        s.camera.zoom  = 4.;
        s.export_settings.format = ImageFormat::WebP;
        apply_preset(s, PresetId::Outdoor);
        CHECK(s.materials.override_for(1) != nullptr);
        CHECK(s.floor.image == std::string(64, 'a'));
        CHECK(s.camera.zoom == 4.);
        CHECK(s.export_settings.format == ImageFormat::WebP);
    }
}

TEST_CASE("Showcase: the bundled environment and floor manifests", "[Showcase][Presets]")
{
    EnvironmentManifest envs;
    REQUIRE(parse_environment_manifest(read_text(resources_path() + "/" + ENVIRONMENTS_DIR + "/" + ENVIRONMENTS_MANIFEST), envs));
    CHECK(envs.environments.size() >= 4);
    CHECK(envs.levels >= 1);
    CHECK(envs.face_size >= 8);
    for (const EnvironmentInfo &e : envs.environments) {
        INFO(e.id);
        CHECK_FALSE(e.file.empty());
        CHECK(e.licence.find("CC0") != std::string::npos);
        CHECK_FALSE(e.source.empty());
        CHECK(std::ifstream(resources_path() + "/" + ENVIRONMENTS_DIR + "/" + e.file).good());
        // Light comes from above in every map: an up-facing normal is lit.
        const auto up = eval_sh(e.sh, { 0.f, 0.f, 1.f });
        CHECK(up[0] + up[1] + up[2] > 0.f);
    }
    FloorManifest floors;
    REQUIRE(parse_floor_manifest(read_text(resources_path() + "/" + FLOORS_DIR + "/" + FLOORS_MANIFEST), floors));
    CHECK(floors.floors.size() >= 3);
    for (const FloorTextureInfo &f : floors.floors) {
        INFO(f.id);
        CHECK(f.licence.find("CC0") != std::string::npos);
        CHECK(f.tile_mm > 0.f);
        CHECK(std::ifstream(resources_path() + "/" + FLOORS_DIR + "/" + f.file).good());
    }
}

TEST_CASE("Showcase: manifests reject bad input", "[Showcase][Presets]")
{
    EnvironmentManifest envs;
    CHECK_FALSE(parse_environment_manifest("", envs));
    CHECK_FALSE(parse_environment_manifest("[]", envs));
    CHECK_FALSE(parse_environment_manifest(R"({"version":1})", envs));
    const std::string sh9 = "[[1,1,1],[0,0,0],[0.5,0.5,0.5],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0],[0,0,0]]";
    REQUIRE(parse_environment_manifest(R"({"face_size":64,"levels":3,"environments":[
        {"id":"ok","file":"ok.png","sh":)" + sh9 + R"(,"sun_dir":[0,0,2]},
        {"id":"escape","file":"../../secret.png","sh":)" + sh9 + R"(},
        {"id":"nosh","file":"x.png"},
        {"id":"ok","file":"dup.png","sh":)" + sh9 + R"(},
        {"file":"noid.png","sh":)" + sh9 + R"(}]})", envs));
    REQUIRE(envs.environments.size() == 1);
    CHECK(envs.face_size == 64);
    CHECK(envs.levels == 3);
    CHECK(envs.environments[0].name == "ok");
    CHECK(envs.environments[0].has_sun);
    CHECK_THAT(envs.environments[0].sun_dir[2], WithinAbs(1., 1e-6));

    FloorManifest floors;
    CHECK_FALSE(parse_floor_manifest("{}", floors));
    REQUIRE(parse_floor_manifest(R"({"floors":[{"id":"a","file":"a.jpg","tile_mm":-4,"roughness":3},{"id":"b","file":"C:\\b.jpg"}]})", floors));
    REQUIRE(floors.floors.size() == 1);
    CHECK(floors.floors[0].tile_mm == 10.f);
    CHECK(floors.floors[0].roughness == 1.f);
}

TEST_CASE("Showcase: environment atlas layout, RGBM and SH", "[Showcase][Presets]")
{
    CHECK(env_atlas_width(128) == 768);
    CHECK(env_atlas_height(128, 5) == 128 + 64 + 32 + 16 + 8);
    CHECK(env_atlas_face(128, 5, 0, 0) == RectI{ 0, 0, 128, 128 });
    CHECK(env_atlas_face(128, 5, 1, 5) == RectI{ 5 * 64, 128, 64, 64 });
    CHECK(env_atlas_face(128, 5, 4, 2) == RectI{ 2 * 8, 128 + 64 + 32 + 16, 8, 8 });
    CHECK(env_atlas_face(128, 5, 5, 0).empty());
    CHECK(env_atlas_face(128, 5, 0, 6).empty());

    const auto c = decode_rgbm(255, 128, 0, 255, 16.f);
    CHECK_THAT(c[0], WithinAbs(16., 1e-5));
    CHECK_THAT(c[1], WithinAbs(16. * 128. / 255., 1e-4));
    CHECK(c[2] == 0.f);
    CHECK(decode_rgbm(255, 255, 255, 0, 16.f)[0] == 0.f);

    // A constant environment of radiance 1: the Lambert term is 1 in every direction.
    std::array<std::array<float, 3>, 9> sh{};
    sh[0] = { 1.f / 0.282095f, 1.f / 0.282095f, 1.f / 0.282095f };
    for (const std::array<float, 3> &n : { std::array<float, 3>{ 0.f, 0.f, 1.f }, std::array<float, 3>{ 1.f, 0.f, 0.f },
                                           std::array<float, 3>{ 0.f, -0.6f, -0.8f } })
        CHECK_THAT(eval_sh(sh, n)[1], WithinAbs(1., 1e-4));

    // World Z up is the cube map's +Y; the two conversions undo each other.
    const auto up = world_to_cube({ 0.f, 0.f, 1.f });
    CHECK(up[1] == 1.f);
    const std::array<float, 3> w{ 0.3f, -0.5f, 0.81f };
    const auto back = cube_to_world(world_to_cube(w));
    CHECK(back == w);
}

// ------------------------------------------------------------------------------------------------
// WebP
// ------------------------------------------------------------------------------------------------

namespace {

uint32_t le32(const std::vector<uint8_t> &b, size_t at)
{
    return uint32_t(b[at]) | (uint32_t(b[at + 1]) << 8) | (uint32_t(b[at + 2]) << 16) | (uint32_t(b[at + 3]) << 24);
}

} // namespace

TEST_CASE("Showcase: WebP export writes a lossless WebP file", "[Showcase][WebP]")
{
    const int w = 37, h = 23;
    std::vector<uint8_t> rgba(size_t(w) * size_t(h) * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &rgba[(size_t(y) * w + x) * 4];
            p[0] = uint8_t(x * 7);
            p[1] = uint8_t(y * 11);
            p[2] = uint8_t(x ^ y);
            p[3] = uint8_t(x < 5 ? 128 : 255);
        }
    const std::vector<uint8_t> file = encode_webp_lossless(rgba, w, h);
    REQUIRE(file.size() > 30);
    CHECK(std::string(file.begin(), file.begin() + 4) == "RIFF");
    CHECK(std::string(file.begin() + 8, file.begin() + 16) == "WEBPVP8L");
    CHECK(le32(file, 4) == file.size() - 8);
    const uint32_t chunk = le32(file, 16);
    CHECK(chunk + (chunk & 1u) + 20 == file.size());
    CHECK(file[20] == 0x2F);   // VP8L signature
    // 14 bits width - 1, 14 bits height - 1, alpha used, version 0.
    const uint32_t hdr = le32(file, 21);
    CHECK((hdr & 0x3FFF) + 1 == uint32_t(w));
    CHECK(((hdr >> 14) & 0x3FFF) + 1 == uint32_t(h));
    CHECK(((hdr >> 28) & 1) == 1);
    CHECK((hdr >> 29) == 0);
    // The same image gives the same bytes.
    CHECK(encode_webp_lossless(rgba, w, h) == file);

    SECTION("an opaque image says so") {
        for (size_t i = 3; i < rgba.size(); i += 4)
            rgba[i] = 255;
        const std::vector<uint8_t> opaque = encode_webp_lossless(rgba, w, h);
        CHECK(((le32(opaque, 21) >> 28) & 1) == 0);
    }
    SECTION("a flat image is tiny") {
        std::vector<uint8_t> flat(size_t(512) * 512 * 4, 200);
        CHECK(encode_webp_lossless(flat, 512, 512).size() < 400);
    }
    SECTION("sizes the format cannot hold give nothing") {
        CHECK(encode_webp_lossless(rgba, 0, h).empty());
        CHECK(encode_webp_lossless(rgba, w, h + 1).empty());   // too few bytes
        CHECK(encode_webp_lossless(std::vector<uint8_t>(16), 16385, 1).empty());
    }
}

TEST_CASE("Showcase: the lights panel's three-point arrangements", "[Showcase][Lighting]")
{
    CHECK(light_rigs().size() == 5);
    for (LightRig rig : light_rigs()) {
        Lighting li;
        li.azimuth_deg     = 123.f;
        li.key_temperature = 3300.f;
        li.environment     = "sunset";
        apply_light_rig(li, rig);
        INFO(int(rig));
        // The key's direction and colour and the environment are the user's.
        CHECK(li.azimuth_deg == 123.f);
        CHECK(li.key_temperature == 3300.f);
        CHECK(li.environment == "sunset");
        CHECK(li.key_intensity > 0.f);
    }
    Lighting li;
    apply_light_rig(li, LightRig::Dramatic);
    CHECK_FALSE(li.fill);
    CHECK(li.rim);
    CHECK(three_point_lights(li).size() == 2);
    apply_light_rig(li, LightRig::Sun);
    CHECK(three_point_lights(li).size() == 1);
    CHECK(li.shadow_softness < 1.f);   // a hard sun shadow
    apply_light_rig(li, LightRig::ClassicThreePoint);
    CHECK(three_point_lights(li).size() == 3);
}
