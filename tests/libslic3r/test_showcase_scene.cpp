// Showcase tab (phase S1): the scene description, its JSON and 3MF storage, the export plan, the
// safe frame and the watermark layout (src/libslic3r/Showcase/ShowcaseScene.{hpp,cpp}); and the
// realistic-view settings struct the Showcase canvas is driven by (RealisticRenderSettings).

#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/RealisticRenderSettings.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/Showcase/ShowcaseScene.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

using namespace Slic3r;
using namespace Slic3r::Showcase;
using Catch::Matchers::WithinAbs;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

fs::path scratch_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / fs::unique_path("snorca_showcase_" + name + "_%%%%-%%%%");
    fs::create_directories(dir);
    return dir;
}

bool zip_has(const std::string &zip_path, const std::string &prefix)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return false;
    bool found = false;
    const mz_uint n = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < n && !found; ++i) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&archive, i, &stat))
            found = std::string(stat.m_filename).rfind(prefix, 0) == 0;
    }
    close_zip_reader(&archive);
    return found;
}

std::string zip_entry(const std::string &zip_path, const std::string &entry_name)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path))
        return {};
    size_t      size = 0;
    void       *data = mz_zip_reader_extract_file_to_heap(&archive, entry_name.c_str(), &size, 0);
    std::string result;
    if (data != nullptr) {
        result.assign(static_cast<const char *>(data), size);
        mz_free(data);
    }
    close_zip_reader(&archive);
    return result;
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(1);
    cfg.set_num_filaments(1);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values  = {0.4};
    cfg.option<ConfigOptionStrings>("filament_colour")->values = {"#FF0000"};
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

void one_cube(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "cube";
    object->add_volume(make_cube(20., 20., 20.));
    object->add_instance();
    object->ensure_on_bed();
}

bool store(const fs::path &path, Model &model, DynamicPrintConfig &cfg, SaveStrategy strategy, BambuExport::Report *bambu = nullptr)
{
    const std::string p = path.string();
    StoreParams       sp;
    sp.path         = p.c_str();
    sp.model        = &model;
    sp.config       = &cfg;
    sp.strategy     = strategy;
    sp.bambu_compat = bambu != nullptr;
    sp.bambu_report = bambu;
    return store_bbs_3mf(sp);
}

bool load(const fs::path &path, Model &model)
{
    DynamicPrintConfig        config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plates;
    std::vector<Preset *>     presets;
    bool                      is_bbl = false;
    Semver                    version;
    const bool ok = load_bbs_3mf(path.string().c_str(), &config, &ctxt, &model, &plates, &presets, &is_bbl, &version, nullptr,
                                 LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances |
                                     LoadStrategy::Silence);
    release_PlateData_list(plates);
    for (Preset *preset : presets)
        delete preset;
    return ok;
}

const SaveStrategy PROJECT = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;

// A tiny but real PNG is not needed: the store keeps bytes verbatim and keys them by hash.
std::vector<uint8_t> fake_image(uint8_t seed, size_t n = 64)
{
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = uint8_t(seed + i * 7);
    return v;
}

Project rich_project()
{
    Project p;
    Scene &a = p.ensure_scene_for_plate(0, PresetId::StudioWhite);
    a.name   = "Hero shot";
    a.background.type  = BackgroundType::Image;
    a.background.image = p.add_image(fake_image(1), "jpg");
    a.background.fit   = ImageFit::Fit;
    a.floor.type       = FloorType::Grid;
    a.floor.grid_spacing = 25.f;
    a.lighting.azimuth_deg   = 120.f;
    a.lighting.elevation_deg = 33.f;
    a.camera.valid  = true;
    a.camera.eye    = { 10., -250., 180. };
    a.camera.target = { 1., 2., 3. };
    a.camera.zoom   = 2.5;
    a.camera.projection = Projection::Ortho;
    CameraView v = a.camera;
    v.name = "Front";
    a.views.push_back(v);
    a.export_settings.width   = 3840;
    a.export_settings.height  = 2160;
    a.export_settings.quality = Quality::Best;
    a.export_settings.format  = ImageFormat::JPEG;
    a.export_settings.watermark.enabled = true;
    a.export_settings.watermark.text    = "Ace Prints";
    a.export_settings.watermark.logo    = p.add_image(fake_image(2), "png");
    a.export_settings.watermark.anchor  = Anchor::TopLeft;

    Scene &b = p.ensure_scene_for_plate(2, PresetId::DarkGradient);
    b.floor.hide_plate = false;
    p.active = b.id;
    // Never referenced: not written.
    p.add_image(fake_image(3), "png");
    return p;
}

} // namespace

// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: a scene survives JSON unchanged", "[Showcase]")
{
    Project p = rich_project();
    Project back;
    std::string error;
    REQUIRE(scenes_from_json(scenes_to_json(p), back, &error));
    CHECK(error.empty());
    REQUIRE(back.scenes.size() == 2);
    CHECK(back.scenes[0] == p.scenes[0]);
    CHECK(back.scenes[1] == p.scenes[1]);
    CHECK(back.active == p.active);
    CHECK(back.schema == SCHEMA_VERSION);
    CHECK_FALSE(back.read_only());
    // Writing again gives the same text: two saves of one project match.
    CHECK(scenes_to_json(back) == scenes_to_json(p));
}

TEST_CASE("Showcase: keys this build does not know are kept", "[Showcase]")
{
    const std::string text = R"({
        "schema": 1, "active": "plate1", "future_top": {"x": 1},
        "scenes": [ { "id": "plate1", "plate_index": 0, "future_scene": [1, 2, 3],
                      "background": { "type": "solid", "color1": "#112233", "future_bg": true },
                      "export": { "width": 800, "height": 600, "watermark": { "enabled": true, "text": "Hi", "future_wm": 5 } } } ] })";
    Project p;
    REQUIRE(scenes_from_json(text, p));
    REQUIRE(p.scenes.size() == 1);
    CHECK(p.scenes[0].background.type == BackgroundType::Solid);
    CHECK(p.scenes[0].background.color1 == "#112233");
    CHECK(p.scenes[0].export_settings.width == 800);
    CHECK(p.scenes[0].export_settings.watermark.text == "Hi");

    p.scenes[0].background.color1 = "#445566";
    const json out = json::parse(scenes_to_json(p));
    CHECK(out["future_top"]["x"] == 1);
    CHECK(out["scenes"][0]["future_scene"].size() == 3);
    CHECK(out["scenes"][0]["background"]["future_bg"] == true);
    CHECK(out["scenes"][0]["background"]["color1"] == "#445566");
    CHECK(out["scenes"][0]["export"]["watermark"]["future_wm"] == 5);
}

TEST_CASE("Showcase: bad input never throws and loads nothing", "[Showcase]")
{
    Project p;
    CHECK_FALSE(scenes_from_json("", p));
    CHECK_FALSE(scenes_from_json("not json", p));
    CHECK_FALSE(scenes_from_json("[1,2]", p));
    CHECK_FALSE(scenes_from_json(R"({"scenes": []})", p));   // no schema
    CHECK(p.empty());

    // Wrong types fall back to defaults, out-of-range values are clamped.
    REQUIRE(scenes_from_json(R"({"schema":1,"scenes":[{"id":"x","plate_index":"two","background":{"type":"plaid","color1":"red"},
        "floor":{"shadow_strength":7},"lighting":{"elevation":-40},"export":{"width":999999,"height":-5,"quality":"ultra"},
        "camera":{"eye":[0,0,0],"target":[0,0,0]}}]})", p));
    REQUIRE(p.scenes.size() == 1);
    const Scene &s = p.scenes[0];
    CHECK(s.plate_index == 0);
    CHECK(s.background.type == Background().type);
    CHECK(s.background.color1 == Background().color1);
    CHECK(s.floor.shadow_strength == 1.f);
    CHECK(s.lighting.elevation_deg == 1.f);
    CHECK(s.export_settings.width == MAX_EXPORT_SIDE);
    CHECK(s.export_settings.height == MIN_EXPORT_SIDE);
    CHECK(s.export_settings.quality == Quality::High);
    CHECK_FALSE(s.camera.valid);   // a camera looking at its own eye
}

TEST_CASE("Showcase: a newer schema is kept read-only and written back verbatim", "[Showcase]")
{
    const std::string text = R"({"schema": 99, "scenes": [{"id": "a", "plate_index": 0, "new_thing": {}}]})";
    Project p;
    REQUIRE(scenes_from_json(text, p));
    CHECK(p.read_only());
    CHECK(p.scenes.size() == 1);
    p.scenes[0].name = "changed";
    CHECK(scenes_to_json(p) == text);
    p.on_plate_deleted(0);   // ignored while read-only
    CHECK(p.scenes.size() == 1);
}

TEST_CASE("Showcase: one scene per plate, and plates deleted under it", "[Showcase]")
{
    Project p;
    Scene &a = p.ensure_scene_for_plate(0);
    Scene &b = p.ensure_scene_for_plate(1, PresetId::BuildPlate);
    CHECK(&p.ensure_scene_for_plate(0) == &p.scenes[0]);
    CHECK(p.scenes.size() == 2);
    CHECK(p.scenes[1].floor.hide_plate == false);
    CHECK(p.scenes[1].preset_base == "build_plate");
    CHECK(p.scenes[0].id != p.scenes[1].id);
    (void) a;
    (void) b;

    p.ensure_scene_for_plate(3);
    p.on_plate_deleted(1);
    REQUIRE(p.scenes.size() == 2);
    CHECK(p.scene_for_plate(0) != nullptr);
    CHECK(p.scene_for_plate(1) == nullptr);
    CHECK(p.scene_for_plate(2) != nullptr);   // was plate 3
}

TEST_CASE("Showcase: presets change the look and keep the camera and export", "[Showcase]")
{
    Scene s = make_scene(PresetId::StudioWhite, 0);
    CHECK(s.background.type == BackgroundType::RadialGradient);
    CHECK(s.floor.type == FloorType::ShadowCatcher);
    CHECK(s.floor.hide_plate);
    CHECK(s.lighting.world_fixed);   // D14
    CHECK_FALSE(s.export_settings.watermark.enabled);   // D12: off by default

    s.camera.valid = true;
    s.camera.zoom  = 3.;
    s.export_settings.width = 1234;
    apply_preset(s, PresetId::DarkGradient);
    CHECK(s.background.type == BackgroundType::VerticalGradient);
    CHECK(s.camera.zoom == 3.);
    CHECK(s.export_settings.width == 1234);
    apply_preset(s, PresetId::BuildPlate);
    CHECK_FALSE(s.floor.hide_plate);
    CHECK(builtin_presets().size() == 3);
    for (PresetId id : builtin_presets()) {
        PresetId back;
        REQUIRE(from_string(to_string(id), back));
        CHECK(back == id);
    }
}

TEST_CASE("Showcase: colours and the light direction", "[Showcase]")
{
    std::array<float, 4> c;
    REQUIRE(parse_hex_color("#FF8000", c));
    CHECK_THAT(c[0], WithinAbs(1., 1e-6));
    CHECK_THAT(c[1], WithinAbs(128. / 255., 1e-6));
    CHECK_THAT(c[3], WithinAbs(1., 1e-6));
    REQUIRE(parse_hex_color("#00000080", c));
    CHECK_THAT(c[3], WithinAbs(128. / 255., 1e-6));
    CHECK_FALSE(parse_hex_color("FF8000", c));
    CHECK_FALSE(parse_hex_color("#GG0000", c));
    CHECK(to_hex_color({ 1.f, 0.5f, 0.f, 1.f }) == "#FF8000");
    CHECK(normalize_hex_color("#abcdef", "#000000") == "#ABCDEF");
    CHECK(normalize_hex_color("nope", "#000000") == "#000000");

    const auto up = light_direction(0.f, 90.f);
    CHECK_THAT(up[2], WithinAbs(1., 1e-3));
    const auto d = light_direction(90.f, 0.f);
    CHECK_THAT(d[0], WithinAbs(0., 1e-9));
    CHECK_THAT(d[1], WithinAbs(1., 1e-9));
    const auto any = light_direction(225.f, 55.f);
    CHECK_THAT(any[0] * any[0] + any[1] * any[1] + any[2] * any[2], WithinAbs(1., 1e-9));
}

TEST_CASE("Showcase: image archive names", "[Showcase]")
{
    const std::string sha(64, 'a');
    CHECK(image_archive_path(sha, "jpg") == "Metadata/showcase/" + sha + ".jpg");
    CHECK(image_archive_path(sha, "png") == "Metadata/showcase/" + sha + ".png");
    std::string s, e;
    REQUIRE(parse_image_archive_path("Metadata/showcase/" + sha + ".JPEG", s, e));
    CHECK(s == sha);
    CHECK(e == "jpg");
    CHECK_FALSE(parse_image_archive_path("Metadata/showcase/scenes.json", s, e));
    CHECK_FALSE(parse_image_archive_path("Metadata/showcase/short.png", s, e));
    CHECK_FALSE(parse_image_archive_path("Metadata/other/" + sha + ".png", s, e));
    CHECK_FALSE(parse_image_archive_path("Metadata/showcase/" + std::string(64, 'z') + ".png", s, e));

    Project p = rich_project();
    CHECK(p.images.size() == 3);
    CHECK(p.used_images().size() == 2);
    p.prune_images();
    CHECK(p.images.size() == 2);
    // The same bytes twice are one image.
    const std::string h1 = p.add_image(fake_image(9), "png");
    const std::string h2 = p.add_image(fake_image(9), "png");
    CHECK(h1 == h2);
    CHECK(h1 == image_fill_sha256_hex(fake_image(9)));
}

// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: scenes round-trip through the project 3MF and stay out of other files", "[Showcase][3MF]")
{
    const fs::path dir = scratch_dir("3mf");
    set_temporary_dir(dir.string());
    DynamicPrintConfig cfg = project_config();

    Model model;
    one_cube(model);
    model.showcase = rich_project();

    SECTION("project save and reopen") {
        const fs::path path = dir / "project.3mf";
        REQUIRE(store(path, model, cfg, PROJECT));
        CHECK(zip_has(path.string(), "Metadata/showcase/scenes.json"));
        Model back;
        REQUIRE(load(path, back));
        REQUIRE(back.showcase.scenes.size() == 2);
        CHECK(back.showcase.scenes[0] == model.showcase.scenes[0]);
        CHECK(back.showcase.scenes[1] == model.showcase.scenes[1]);
        CHECK(back.showcase.active == model.showcase.active);
        // Only the two images a scene uses were written, bytes intact.
        CHECK(back.showcase.images.size() == 2);
        for (const std::string &sha : model.showcase.used_images()) {
            const ImageBlob *a = model.showcase.find_image(sha);
            const ImageBlob *b = back.showcase.find_image(sha);
            REQUIRE(a != nullptr);
            REQUIRE(b != nullptr);
            CHECK(*a == *b);
        }
        // The plate thumbnails reader must not have taken the images.
        const fs::path again = dir / "again.3mf";
        REQUIRE(store(again, back, cfg, PROJECT));
        Model twice;
        REQUIRE(load(again, twice));
        CHECK(twice.showcase == back.showcase);
    }

    SECTION("a project without scenes writes none") {
        Model plain;
        one_cube(plain);
        const fs::path path = dir / "plain.3mf";
        REQUIRE(store(path, plain, cfg, PROJECT));
        CHECK_FALSE(zip_has(path.string(), "Metadata/showcase/"));
        Model back;
        REQUIRE(load(path, back));
        CHECK(back.showcase.empty());
    }

    SECTION("Export Bambu 3MF leaves the scenes out, also after a reopen") {
        const fs::path project = dir / "project.3mf";
        REQUIRE(store(project, model, cfg, PROJECT));
        Model reopened;
        REQUIRE(load(project, reopened));
        REQUIRE_FALSE(reopened.showcase.empty());
        for (Model *m : { &model, &reopened }) {
            BambuExport::Report report;
            const fs::path      path = dir / "bambu.3mf";
            REQUIRE(store(path, *m, cfg, PROJECT, &report));
            CHECK_FALSE(zip_has(path.string(), "Metadata/showcase/"));
            Model back;
            REQUIRE(load(path, back));
            CHECK(back.showcase.empty());
        }
    }

    SECTION("a sliced-plate file leaves them out, a backup keeps them") {
        const fs::path plate = dir / "plate.gcode.3mf";
        REQUIRE(store(plate, model, cfg, PROJECT | SaveStrategy::WithGcode | SaveStrategy::SkipModel));
        CHECK_FALSE(zip_has(plate.string(), "Metadata/showcase/"));
        const fs::path backup = dir / "backup.3mf";
        REQUIRE(store(backup, model, cfg, PROJECT | SaveStrategy::Backup));
        CHECK(zip_has(backup.string(), "Metadata/showcase/scenes.json"));
    }

    SECTION("a damaged scenes file opens the project without scenes") {
        // Write a project, then a second one whose scenes.json is not JSON: same code path as a
        // hand-edited or truncated file. The model itself still loads.
        Model broken;
        one_cube(broken);
        broken.showcase.ensure_scene_for_plate(0);
        broken.showcase.extra_json = "";   // fine
        const fs::path path = dir / "ok.3mf";
        REQUIRE(store(path, broken, cfg, PROJECT));
        CHECK_FALSE(zip_entry(path.string(), "Metadata/showcase/scenes.json").empty());
        Project p;
        CHECK_FALSE(scenes_from_json(zip_entry(path.string(), "Metadata/showcase/scenes.json").substr(0, 10), p));
        Model back;
        REQUIRE(load(path, back));
        CHECK(back.objects.size() == 1);
        CHECK(back.showcase.scenes.size() == 1);
    }

    SECTION("a newer schema survives a save by this build") {
        Model future;
        one_cube(future);
        const std::string text = R"({"schema": 7, "scenes": [{"id": "x", "plate_index": 0, "hdr": "env_1"}]})";
        REQUIRE(scenes_from_json(text, future.showcase));
        future.showcase.add_image(fake_image(5), "png");
        const fs::path path = dir / "future.3mf";
        REQUIRE(store(path, future, cfg, PROJECT));
        CHECK(zip_entry(path.string(), "Metadata/showcase/scenes.json") == text);
        Model back;
        REQUIRE(load(path, back));
        CHECK(back.showcase.read_only());
        CHECK(back.showcase.images.size() == 1);   // kept, though no scene names it
    }

    SECTION("a copied model carries its scenes") {
        Model copy(model);
        CHECK(copy.showcase == model.showcase);
    }

    boost::system::error_code ec;
    fs::remove_all(dir, ec);   // best effort: a backup folder may still be held open
}

// ------------------------------------------------------------------------------------------------

TEST_CASE("Showcase: export plans obey the size cap and the memory budget", "[Showcase]")
{
    SECTION("1080p High: 2x supersampling plus MSAA") {
        const ExportPlan p = plan_export(1920, 1080, Quality::High, 8, 540);
        CHECK(p.ssaa == 2);
        CHECK(p.msaa == 4);
        CHECK(p.internal_width == 3840);
        CHECK(p.internal_height == 2160);
        CHECK_FALSE(p.degraded);
        CHECK_THAT(p.ssao_radius_scale, WithinAbs(4., 1e-6));   // 2160 / 540
    }
    SECTION("4K High: the 1 GB budget drops MSAA, keeps 2x") {
        const ExportPlan p = plan_export(3840, 2160, Quality::High, 8, 1000);
        CHECK(p.ssaa == 2);
        CHECK(p.msaa == 0);
        CHECK(p.internal_width == 7680);
        CHECK(p.degraded);
        CHECK(p.estimated_bytes <= double(size_t(1) << 30));
    }
    SECTION("4K Best: 3x would exceed 8192 per side") {
        const ExportPlan p = plan_export(3840, 2160, Quality::Best, 4, 1000);
        CHECK(p.ssaa == 2);
        CHECK(p.internal_width <= MAX_EXPORT_SIDE);
        CHECK(p.degraded);
    }
    SECTION("8K output renders 1x") {
        const ExportPlan p = plan_export(8192, 4608, Quality::High, 4, 1000, size_t(4) << 30);
        CHECK(p.ssaa == 1);
        CHECK(p.internal_width == 8192);
    }
    SECTION("sizes are clamped and drivers without MSAA get none") {
        const ExportPlan p = plan_export(100000, 2, Quality::Draft, 1, 0);
        CHECK(p.width == MAX_EXPORT_SIDE);
        CHECK(p.height == MIN_EXPORT_SIDE);
        CHECK(p.msaa == 0);
        CHECK(p.ssaa == 1);
        CHECK(p.ssao_radius_scale == 1.f);
    }
    SECTION("a small GPU texture limit caps the internal size") {
        const ExportPlan p = plan_export(3000, 2000, Quality::High, 4, 1000, size_t(1) << 30, 4096);
        CHECK(p.width == 3000);
        CHECK(p.ssaa == 1);
        CHECK(p.shadow_map_size == 4096);
    }
    CHECK(ssaa_factor(Quality::Draft) == 1);
    CHECK(ssaa_factor(Quality::High) == 2);
    CHECK(ssaa_factor(Quality::Best) == 3);
}

TEST_CASE("Showcase: the safe frame letterboxes the export aspect", "[Showcase]")
{
    // 16:9 in a wide viewport: pillarbox.
    RectI r = fit_frame(2000, 900, 1920, 1080);
    CHECK(r.h == 900);
    CHECK(r.w == 1600);
    CHECK(r.x == 200);
    CHECK(r.y == 0);
    // 9:16 phone format in a landscape viewport.
    r = fit_frame(1600, 900, 1080, 1920);
    CHECK(r.h == 900);
    CHECK(r.w == 506);
    CHECK(r.x == (1600 - 506) / 2);
    // Square with a margin in a tall viewport: letterbox.
    r = fit_frame(800, 1200, 1, 1, 20);
    CHECK(r.w == 760);
    CHECK(r.h == 760);
    CHECK(r.x == 20);
    CHECK(r.y == (1200 - 760) / 2);
    CHECK(fit_frame(0, 100, 16, 9).empty());
    CHECK(fit_frame(100, 100, 0, 9).empty());
}

TEST_CASE("Showcase: watermark layout", "[Showcase][Watermark]")
{
    Watermark wm;
    wm.enabled    = true;
    wm.size_pct   = 10.f;   // 108 px block on 1080p
    wm.margin_pct = 5.f;    // 54 px
    const int W = 1920, H = 1080;

    SECTION("logo and text, bottom right") {
        wm.anchor = Anchor::BottomRight;
        const int font = watermark_font_px(wm, H, true);
        CHECK(font == 54);
        const WatermarkLayout l = layout_watermark(wm, W, H, 2.0, 300, 60);
        CHECK(l.logo.h == 108);
        CHECK(l.logo.w == 216);
        CHECK(l.block.h == 108);
        CHECK(l.block.w == 216 + 32 + 300);   // gap 0.3 x 108
        CHECK(l.block.x + l.block.w == W - 54);
        CHECK(l.block.y + l.block.h == H - 54);
        CHECK(l.text.x == l.logo.x + l.logo.w + 32);
        CHECK(l.text.y == l.block.y + (108 - 60) / 2);   // centred on the logo
    }
    SECTION("text only, top left and centre") {
        wm.anchor = Anchor::TopLeft;
        CHECK(watermark_font_px(wm, H, false) == 81);
        WatermarkLayout l = layout_watermark(wm, W, H, 0., 400, 90);
        CHECK(l.logo.empty());
        CHECK(l.block == RectI{ 54, 54, 400, 90 });
        CHECK(l.text == l.block);
        wm.anchor = Anchor::Center;
        l = layout_watermark(wm, W, H, 0., 400, 90);
        CHECK(l.block.x == (W - 400) / 2);
        CHECK(l.block.y == (H - 90) / 2);
        wm.anchor = Anchor::Top;
        l = layout_watermark(wm, W, H, 0., 400, 90);
        CHECK(l.block.x == (W - 400) / 2);
        CHECK(l.block.y == 54);
    }
    SECTION("logo only, left middle") {
        wm.anchor = Anchor::Left;
        const WatermarkLayout l = layout_watermark(wm, W, H, 1.0, 0, 0);
        CHECK(l.text.empty());
        CHECK(l.logo == RectI{ 54, (H - 108) / 2, 108, 108 });
    }
    SECTION("a mark wider than the image shrinks to fit inside the margins") {
        wm.anchor = Anchor::BottomLeft;
        const WatermarkLayout l = layout_watermark(wm, 400, 300, 1.0, 2000, 40);
        CHECK(l.block.w <= 400 - 2 * 15);
        CHECK(l.block.x >= 0);
        CHECK(l.block.x + l.block.w <= 400);
        CHECK(l.block.y + l.block.h <= 300);
        CHECK(l.font_px < watermark_font_px(wm, 300, true));
    }
    SECTION("nothing to draw") {
        const WatermarkLayout l = layout_watermark(wm, W, H, 0., 0, 0);
        CHECK(l.block.empty());
        CHECK_FALSE(wm.has_content());
    }
    Anchor a;
    REQUIRE(from_string("bottom_right", a));
    CHECK(a == Anchor::BottomRight);
}

TEST_CASE("Showcase: a watermark preset round-trips on its own", "[Showcase][Watermark]")
{
    Watermark wm;
    wm.name       = "Shop";
    wm.enabled    = true;
    wm.text       = "Ace Prints";
    wm.font_face  = "Segoe UI";
    wm.bold       = true;
    wm.text_color = "#102030";
    wm.logo       = std::string(64, 'b');
    wm.anchor     = Anchor::Top;
    wm.size_pct   = 12.5f;
    wm.opacity    = 0.5f;
    wm.margin_pct = 1.f;
    Watermark back;
    REQUIRE(watermark_from_json(watermark_to_json(wm), back));
    CHECK(back == wm);
    CHECK_FALSE(watermark_from_json("[]", back));
}

TEST_CASE("Showcase: image helpers", "[Showcase]")
{
    // 2x2 -> 1x1 box filter.
    std::vector<uint8_t> img = { 0, 0, 0, 255,   255, 255, 255, 255,   0, 0, 0, 255,   255, 255, 255, 255 };
    const std::vector<uint8_t> small = downsample_rgba(img, 2, 2, 2);
    REQUIRE(small.size() == 4);
    CHECK(int(small[0]) == 128);
    CHECK(int(small[3]) == 255);
    CHECK(downsample_rgba(img, 2, 2, 1) == img);

    // Flip: row 0 <-> row 1.
    std::vector<uint8_t> rows = { 1, 1, 1, 1,   2, 2, 2, 2 };
    flip_rows_rgba(rows, 1, 2);
    CHECK(rows[0] == 2);
    CHECK(rows[4] == 1);

    // Premultiplied half-transparent grey back to straight alpha.
    std::vector<uint8_t> pm = { 64, 64, 64, 128 };
    unpremultiply_rgba(pm);
    CHECK(int(pm[0]) == 128);
    CHECK(int(pm[3]) == 128);

    // White over black at half opacity.
    std::vector<uint8_t> dst = { 0, 0, 0, 255,   0, 0, 0, 255 };
    const std::vector<uint8_t> src = { 255, 255, 255, 255 };
    blend_over_rgba(dst, 2, 1, src, 1, 1, 1, 0, 0.5f);
    CHECK(int(dst[0]) == 0);
    CHECK(int(dst[4]) == 128);
    CHECK(int(dst[7]) == 255);
    // Off the image: nothing happens.
    blend_over_rgba(dst, 2, 1, src, 1, 1, 5, 5, 1.f);
    CHECK(int(dst[4]) == 128);

    std::vector<uint8_t> clear = { 0, 0, 0, 0 };
    flatten_rgba(clear, 255, 255, 255);
    CHECK(int(clear[0]) == 255);
    CHECK(int(clear[3]) == 255);
}

TEST_CASE("Realistic view settings read the preferences as before", "[Showcase][RenderSettings]")
{
    AppConfig config;
    RealisticRenderSettings s = RealisticRenderSettings::from_app_config(config);
    // The shipped defaults (AppConfig::set_defaults): off, Phong when on, no shadows or SSAO.
    CHECK_FALSE(s.realistic);
    CHECK(s.phong);
    CHECK_FALSE(s.shadows);
    CHECK_FALSE(s.ssao);
    // What only the Showcase canvas sets keeps the editing views' behaviour.
    CHECK_FALSE(s.world_light);
    CHECK(s.plate_shadow);
    CHECK(s.shadow_map_size == 2048u);
    CHECK(s.ssao_radius_scale == 1.f);
    CHECK_FALSE(s.ssao_keep_alpha);
    CHECK_FALSE(s.has_fit_box());

    config.set_bool(SETTING_OPENGL_REALISTIC_MODE, true);
    config.set_bool(SETTING_OPENGL_REALISTIC_PHONG, false);
    config.set_bool(SETTING_OPENGL_PHONG_BASIC_PLATE_SHADOWS, true);
    config.set_bool(SETTING_OPENGL_PHONG_SSAO, true);
    s = RealisticRenderSettings::from_app_config(config);
    CHECK(s.realistic);
    CHECK_FALSE(s.phong);
    CHECK(s.shadows);
    CHECK(s.ssao);
}
