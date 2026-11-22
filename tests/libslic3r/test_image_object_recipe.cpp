// "Image to object", re-editing (tests/design_image_to_objects.md, 3.6): the settings and the
// image kept with the objects, their file in the project, and the picture of the 3D preview.

#include <catch2/catch.hpp>

#include <cereal/archives/binary.hpp>

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/BambuExport.hpp"
#include "libslic3r/ImageObjectRecipe.hpp"
#include "libslic3r/MeshPreview.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r/miniz_extension.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

const char *IMAGE_OBJECTS_ENTRY = "Metadata/edgeslicer_image_objects.json";

ImageToObjectSettings changed_settings()
{
    ImageToObjectSettings s;
    s.type                    = ImageObjectType::Clicker;
    s.shape_from              = TraceShapeFrom::Brightness;
    s.threshold               = 77;
    s.auto_threshold          = false;
    s.invert                  = true;
    s.blur                    = 2.25;
    s.silhouette.longest_side = 64.5;
    s.silhouette.close_gaps   = 0.7;
    s.silhouette.min_feature  = 2.1;
    s.silhouette.fill_holes   = false;
    s.silhouette.largest_only = false;
    s.cutter_mirror           = false;
    s.cutter.blade_height     = 15.5;
    s.cutter.blade_thickness  = 1.2;
    s.cutter.handle_width     = 6.;
    s.cutter.handle_height    = 2.5;
    s.cutter.sharp_edge       = false;
    s.cutter.braces           = CutterBraces::Never;
    s.box.height              = 31.;
    s.box.wall                = 2.4;
    s.box.lid_overhang        = 0.5;
    s.box.clearance           = 0.35;
    s.box.lip_height          = 5.5;
    s.clicker.body_height     = 14.;
    s.clicker.socket_fit      = 0.2;
    s.clicker.stem_fit        = 0.1;
    s.clicker.switch_offset   = Vec2d(3., -2.);
    s.clicker.cap_overhang    = 2.5;
    s.clicker.keyring         = true;
    s.clicker.keyring_angle   = 135.;
    s.picture_colors          = 5;
    s.body_extruder           = 2;
    s.level_extruders         = {3, 1, 4, 2, 5, 0, 0, 0};
    clamp_image_object_settings(s);
    return s;
}

// Bytes of every value, in a length which needs the given padding of base64
std::string some_bytes(size_t length)
{
    std::string out;
    for (size_t i = 0; i < length; ++i)
        out += char((i * 37 + 11) & 255);
    return out;
}

ImageObjectRecipe recipe_of(const std::string &group, int index, const std::string &png)
{
    ImageObjectRecipe r;
    r.group      = group;
    r.index      = index;
    r.image_name = "duck";
    r.settings   = image_object_settings_to_json(changed_settings());
    r.source_png = png;
    return r;
}

size_t count_of(const std::string &text, const std::string &what)
{
    size_t n = 0;
    for (size_t pos = text.find(what); pos != std::string::npos; pos = text.find(what, pos + 1))
        ++n;
    return n;
}

ModelObject *cube_object(Model &model, const std::string &name)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    object->add_volume(make_cube(20., 20., 10.));
    object->add_instance();
    object->ensure_on_bed();
    return object;
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

std::string temp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() /
                                             ("snorca_tests_imageobject_" + std::to_string(get_current_pid()));
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    return (tmp_root / name).string();
}

DynamicPrintConfig project_config()
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (const std::string &key : cfg.keys())
        if (const ConfigOption *opt = cfg.option(key); opt != nullptr && opt->type() == coEnums) {
            cfg.erase(key);
            cfg.option(key, true);
        }
    cfg.set_num_extruders(2);
    cfg.set_num_filaments(3);
    cfg.option<ConfigOptionFloats>("nozzle_diameter")->values             = std::vector<double>(2, 0.4);
    cfg.option<ConfigOptionStrings>("filament_colour")->values            = {"#E53935", "#1E88E5", "#43A047"};
    cfg.option<ConfigOptionStrings>("filament_settings_id", true)->values = {"Generic PLA", "Generic PLA", "Generic PLA"};
    cfg.option<ConfigOptionStrings>("filament_type", true)->values        = {"PLA", "PLA", "PLA"};
    cfg.set_key_value("layer_change_gcode", new ConfigOptionString("G92 E0"));
    cfg.set_key_value("printer_model", new ConfigOptionString("Bambu Lab P1S"));
    return cfg;
}

PlateDataPtrs one_plate(size_t objects)
{
    PlateData *a   = new PlateData();
    a->plate_index = 0;
    for (size_t i = 0; i < objects; ++i)
        a->objects_and_instances.emplace_back(int(i), 0);
    return {a};
}

bool store(const std::string &path, Model &model, bool bambu)
{
    DynamicPrintConfig  cfg = project_config();
    BambuExport::Report report;
    StoreParams         sp;
    sp.path            = path.c_str();
    sp.model           = &model;
    sp.config          = &cfg;
    sp.plate_data_list = one_plate(model.objects.size());
    sp.strategy        = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    sp.bambu_compat    = bambu;
    sp.bambu_report    = bambu ? &report : nullptr;
    return store_bbs_3mf(sp);
}

struct Loaded
{
    bool                  ok = false;
    Model                 model;
    DynamicPrintConfig    config;
    PlateDataPtrs         plates;
    std::vector<Preset *> presets;
    bool                  is_bbl_3mf = false;
    Semver                version;
    ~Loaded()
    {
        release_PlateData_list(plates);
        for (Preset *p : presets)
            delete p;
    }
};

void load(const std::string &path, Loaded &out)
{
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    LoadStrategy strategy = LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances | LoadStrategy::Silence | LoadStrategy::LoadConfig;
    out.ok = load_bbs_3mf(path.c_str(), &out.config, &ctxt, &out.model, &out.plates, &out.presets, &out.is_bbl_3mf, &out.version, nullptr,
                          strategy);
}

int alpha_at(const MeshPreviewImage &image, int x, int y) { return image.rgba[(size_t(y) * size_t(image.width) + size_t(x)) * 4 + 3]; }

} // namespace

TEST_CASE("Image object settings: the JSON round-trips", "[ImageObjectRecipe][Json]")
{
    const ImageToObjectSettings s    = changed_settings();
    const std::string           text = image_object_settings_to_json(s);
    ImageToObjectSettings       r;
    REQUIRE(image_object_settings_from_json(text, r));

    CHECK(r.type == ImageObjectType::Clicker);
    CHECK(r.shape_from == TraceShapeFrom::Brightness);
    CHECK(r.threshold == 77);
    CHECK_FALSE(r.auto_threshold);
    CHECK(r.invert);
    CHECK_THAT(r.blur, WithinAbs(2.25, 1e-9));
    CHECK_THAT(r.silhouette.longest_side, WithinAbs(64.5, 1e-9));
    CHECK_THAT(r.silhouette.close_gaps, WithinAbs(0.7, 1e-9));
    CHECK_THAT(r.silhouette.min_feature, WithinAbs(2.1, 1e-9));
    CHECK_FALSE(r.silhouette.fill_holes);
    CHECK_FALSE(r.silhouette.largest_only);
    CHECK_FALSE(r.cutter_mirror);
    CHECK_THAT(r.cutter.blade_height, WithinAbs(15.5, 1e-9));
    CHECK_THAT(r.cutter.blade_thickness, WithinAbs(1.2, 1e-9));
    CHECK_FALSE(r.cutter.sharp_edge);
    CHECK(r.cutter.braces == CutterBraces::Never);
    CHECK_THAT(r.box.height, WithinAbs(31., 1e-9));
    CHECK_THAT(r.box.wall, WithinAbs(2.4, 1e-9));
    CHECK_THAT(r.box.clearance, WithinAbs(0.35, 1e-9));
    CHECK_THAT(r.box.lip_height, WithinAbs(5.5, 1e-9));
    CHECK_THAT(r.clicker.body_height, WithinAbs(14., 1e-9));
    CHECK_THAT(r.clicker.socket_fit, WithinAbs(0.2, 1e-9));
    CHECK_THAT(r.clicker.switch_offset.x(), WithinAbs(3., 1e-9));
    CHECK_THAT(r.clicker.switch_offset.y(), WithinAbs(-2., 1e-9));
    CHECK(r.clicker.keyring);
    CHECK_THAT(r.clicker.keyring_angle, WithinAbs(135., 1e-9));
    CHECK(r.picture_colors == 5);
    CHECK(r.body_extruder == 2);
    CHECK(r.level_extruders == s.level_extruders);
    // what follows from the other values
    CHECK(r.box.decoration);
    CHECK(r.clicker.decoration);
    CHECK_FALSE(r.silhouette.mirror);

    // the same settings give the same text, so a saved project does not change by itself
    CHECK(image_object_settings_to_json(r) == text);
}

TEST_CASE("Image object settings: bad text is refused or limited", "[ImageObjectRecipe][Json]")
{
    ImageToObjectSettings s;
    CHECK_FALSE(image_object_settings_from_json("", s));
    CHECK_FALSE(image_object_settings_from_json("not json", s));
    CHECK_FALSE(image_object_settings_from_json("[1, 2, 3]", s));
    CHECK_FALSE(image_object_settings_from_json("42", s));

    // values out of range are limited, values of a wrong kind keep the default
    const ImageToObjectSettings defaults;
    REQUIRE(image_object_settings_from_json(R"({"type": 99, "size": 1e9, "blur": -4, "threshold": "dark", "invert": 1,
                                                "box": {"height": 0, "wall": "thick"}, "clicker": [1, 2],
                                                "picture_colors": 1, "body_filament": -7, "level_filaments": [300, "x", 2]})",
                                            s));
    CHECK(s.type == ImageObjectType::Clicker);
    CHECK_THAT(s.silhouette.longest_side, WithinAbs(1000., 1e-9));
    CHECK_THAT(s.blur, WithinAbs(0., 1e-9));
    CHECK(s.threshold == defaults.threshold);
    CHECK(s.invert == defaults.invert);
    CHECK_THAT(s.box.height, WithinAbs(3., 1e-9));
    CHECK_THAT(s.box.wall, WithinAbs(defaults.box.wall, 1e-9));
    CHECK_THAT(s.clicker.body_height, WithinAbs(defaults.clicker.body_height, 1e-9));
    // one colour is no picture
    CHECK(s.picture_colors == 0);
    CHECK_FALSE(s.box.decoration);
    CHECK(s.body_extruder == 0);
    CHECK(s.level_extruders[0] == 255);
    CHECK(s.level_extruders[1] == 0);
    CHECK(s.level_extruders[2] == 2);

    // an empty object is the defaults
    ImageToObjectSettings e;
    REQUIRE(image_object_settings_from_json("{}", e));
    CHECK(e.type == defaults.type);
    CHECK_THAT(e.silhouette.longest_side, WithinAbs(defaults.silhouette.longest_side, 1e-9));
}

TEST_CASE("Image object recipes: the file of the project round-trips", "[ImageObjectRecipe][Json]")
{
    CHECK(is_valid_image_object_group(new_image_object_group()));
    CHECK(new_image_object_group() != new_image_object_group());
    CHECK_FALSE(is_valid_image_object_group(""));
    CHECK_FALSE(is_valid_image_object_group("abc"));
    CHECK_FALSE(is_valid_image_object_group("../../etc/passwd"));
    CHECK_FALSE(is_valid_image_object_group("ABCDEF0123"));

    // no recipe, no file
    const ImageObjectRecipe none;
    CHECK(image_object_recipes_to_json({}).empty());
    CHECK(image_object_recipes_to_json({&none, nullptr}).empty());

    // every length of the image modulo 3, so every padding of the encoding
    auto length = GENERATE(size_t(1), size_t(2), size_t(3), size_t(1000), size_t(1001), size_t(1002));
    const std::string       png = some_bytes(length);
    const ImageObjectRecipe base = recipe_of("0123456789abcdef", 0, png);
    const ImageObjectRecipe lid  = recipe_of("0123456789abcdef", 1, png);
    const ImageObjectRecipe other = recipe_of("fedcba9876543210", 0, {});

    const std::string text = image_object_recipes_to_json({&none, &base, nullptr, &lid, &other});
    REQUIRE_FALSE(text.empty());
    // the image of the group is written once
    CHECK(count_of(text, "\"0123456789abcdef\":") == 1);

    const auto back = image_object_recipes_from_json(text);
    REQUIRE(back.size() == 3);
    CHECK(back[0].first == 2);
    CHECK(back[0].second == base);
    CHECK(back[1].first == 4);
    CHECK(back[1].second == lid);
    CHECK(back[2].first == 5);
    CHECK(back[2].second == other);
    CHECK(back[2].second.source_png.empty());
}

TEST_CASE("Image object recipes: bad files are dropped piece by piece", "[ImageObjectRecipe][Json]")
{
    CHECK(image_object_recipes_from_json("").empty());
    CHECK(image_object_recipes_from_json("garbage").empty());
    CHECK(image_object_recipes_from_json("[]").empty());
    CHECK(image_object_recipes_from_json(R"({"objects": 3})").empty());

    const std::string text = R"({"version": 1,
        "images": {"0123456789abcdef": "not base64!", "aaaaaaaaaaaaaaaa": "QUJD", "../evil": "QUJD", "bbbbbbbbbbbbbbbb": "QUI=QUJD"},
        "objects": [
            {"object": 1, "group": "0123456789abcdef", "index": 0, "name": "a", "settings": {"type": 1}},
            {"object": 2, "group": "aaaaaaaaaaaaaaaa", "index": 99, "name": "b", "settings": {}},
            {"object": 2, "group": "aaaaaaaaaaaaaaaa", "index": 1, "name": "twice", "settings": {}},
            {"object": 0, "group": "aaaaaaaaaaaaaaaa", "settings": {}},
            {"object": 3, "group": "../evil", "settings": {}},
            {"object": 4, "group": "aaaaaaaaaaaaaaaa", "settings": "none"},
            {"object": 5, "group": "bbbbbbbbbbbbbbbb", "name": 7, "settings": {}},
            {"object": 6, "settings": {}},
            "text", 12
        ]})";
    const auto back = image_object_recipes_from_json(text);
    REQUIRE(back.size() == 3);
    // an image which cannot be read: the recipe stays, the image is asked for again
    CHECK(back[0].first == 1);
    CHECK(back[0].second.source_png.empty());
    ImageToObjectSettings s;
    REQUIRE(image_object_settings_from_json(back[0].second.settings, s));
    CHECK(s.type == ImageObjectType::Box);
    // a place in the group out of range is limited; the second entry of the same object is dropped
    CHECK(back[1].first == 2);
    CHECK(back[1].second.index == 15);
    CHECK(back[1].second.image_name == "b");
    CHECK(back[1].second.source_png == "ABC");
    // padding in the middle is no image
    CHECK(back[2].first == 5);
    CHECK(back[2].second.image_name.empty());
    CHECK(back[2].second.source_png.empty());
}

TEST_CASE("Image object recipes: copies and undo keep the recipe", "[ImageObjectRecipe][Copy]")
{
    Model        model;
    ModelObject *object         = cube_object(model, "lid");
    object->image_object_recipe = recipe_of("0123456789abcdef", 1, some_bytes(500));

    ModelObject *copy = model.add_object(*object);
    CHECK(copy->image_object_recipe == object->image_object_recipe);

    // what the undo stack does with an object
    std::stringstream stream;
    {
        cereal::BinaryOutputArchive archive(stream);
        archive(object->image_object_recipe);
    }
    ImageObjectRecipe back;
    {
        cereal::BinaryInputArchive archive(stream);
        archive(back);
    }
    CHECK(back == object->image_object_recipe);
}

TEST_CASE("Image object recipes: project round trip", "[ImageObjectRecipe][3mf]")
{
    Model model;
    cube_object(model, "plain");
    ModelObject *base         = cube_object(model, "duck - Box");
    ModelObject *lid          = cube_object(model, "duck - Lid");
    const std::string png     = some_bytes(4000);
    base->image_object_recipe = recipe_of("0123456789abcdef", 0, png);
    lid->image_object_recipe  = recipe_of("0123456789abcdef", 1, png);

    const std::string path = temp_file("imageobject_native.3mf");
    REQUIRE(store(path, model, false));
    CHECK_FALSE(zip_entry(path, IMAGE_OBJECTS_ENTRY).empty());

    Loaded back;
    load(path, back);
    REQUIRE(back.ok);
    REQUIRE(back.model.objects.size() == 3);
    CHECK(back.model.objects[0]->image_object_recipe.empty());
    CHECK(back.model.objects[1]->image_object_recipe == base->image_object_recipe);
    CHECK(back.model.objects[2]->image_object_recipe == lid->image_object_recipe);
    boost::filesystem::remove(path);
}

TEST_CASE("Image object recipes: no recipe, no file; not in a Bambu export", "[ImageObjectRecipe][3mf]")
{
    Model        model;
    ModelObject *object = cube_object(model, "plain");

    const std::string plain = temp_file("imageobject_none.3mf");
    REQUIRE(store(plain, model, false));
    CHECK(zip_entry(plain, IMAGE_OBJECTS_ENTRY).empty());
    Loaded back;
    load(plain, back);
    REQUIRE(back.ok);
    REQUIRE(back.model.objects.size() == 1);
    CHECK(back.model.objects[0]->image_object_recipe.empty());
    boost::filesystem::remove(plain);

    object->image_object_recipe = recipe_of("0123456789abcdef", 0, some_bytes(300));
    const std::string bambu     = temp_file("imageobject_bambu.3mf");
    REQUIRE(store(bambu, model, true));
    CHECK(zip_entry(bambu, IMAGE_OBJECTS_ENTRY).empty());
    boost::filesystem::remove(bambu);
}

TEST_CASE("Mesh preview: a cube is drawn in the middle of the picture", "[MeshPreview]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    MeshPreviewItem            item;
    item.mesh  = &cube;
    item.color = {200, 100, 50};

    SECTION("from above it is a square with the margin around it")
    {
        MeshPreviewImage image = render_mesh_preview({item}, 100, 100, 0., 90.);
        REQUIRE_FALSE(image.empty());
        CHECK(alpha_at(image, 50, 50) == 255);
        CHECK(alpha_at(image, 10, 10) == 255);
        CHECK(alpha_at(image, 90, 90) == 255);
        CHECK(alpha_at(image, 1, 1) == 0);
        CHECK(alpha_at(image, 98, 50) == 0);
        // the colour of the item, darker or equal by the light
        const uint8_t *px = &image.rgba[(size_t(50) * 100 + 50) * 4];
        CHECK(px[0] > px[1]);
        CHECK(px[1] > px[2]);
        CHECK(px[0] <= 200);
        CHECK(px[0] > 60);
    }

    SECTION("turned, three faces with three shades are seen")
    {
        MeshPreviewImage image = render_mesh_preview({item}, 120, 120, -30., 35.);
        REQUIRE_FALSE(image.empty());
        CHECK(alpha_at(image, 60, 60) == 255);
        CHECK(alpha_at(image, 2, 2) == 0);
        CHECK(alpha_at(image, 117, 2) == 0);
        std::vector<int> reds;
        for (size_t i = 0; i < image.rgba.size(); i += 4)
            if (image.rgba[i + 3] == 255 && std::find(reds.begin(), reds.end(), int(image.rgba[i])) == reds.end())
                reds.push_back(int(image.rgba[i]));
        CHECK(reds.size() >= 3);
    }

    SECTION("a second item beside it widens the scene")
    {
        MeshPreviewItem other = item;
        other.offset          = Vec3f(30.f, 0.f, 0.f);
        other.color           = {10, 10, 250};
        MeshPreviewImage image = render_mesh_preview({item, other}, 200, 100, 0., 90.);
        REQUIRE_FALSE(image.empty());
        // red on the left, blue on the right, nothing between them
        const int y = 50;
        CHECK(image.rgba[(size_t(y) * 200 + 30) * 4] > image.rgba[(size_t(y) * 200 + 30) * 4 + 2]);
        CHECK(image.rgba[(size_t(y) * 200 + 170) * 4 + 2] > image.rgba[(size_t(y) * 200 + 170) * 4]);
        CHECK(alpha_at(image, 100, y) == 0);
    }
}

TEST_CASE("Mesh preview: bad input gives an empty picture, not a crash", "[MeshPreview]")
{
    CHECK(render_mesh_preview({}, 0, 100, 0., 30.).empty());
    CHECK(render_mesh_preview({}, 100, 100000, 0., 30.).empty());

    // nothing to draw: a transparent picture
    MeshPreviewImage blank = render_mesh_preview({}, 40, 30, 0., 30.);
    REQUIRE_FALSE(blank.empty());
    CHECK(std::all_of(blank.rgba.begin(), blank.rgba.end(), [](uint8_t v) { return v == 0; }));

    // triangles of points which do not exist are skipped
    indexed_triangle_set broken = its_make_cube(5., 5., 5.);
    broken.indices.emplace_back(0, 1, 5000);
    broken.indices.emplace_back(-3, 1, 2);
    MeshPreviewItem item;
    item.mesh              = &broken;
    MeshPreviewItem  empty; // no mesh at all
    MeshPreviewImage image = render_mesh_preview({item, empty}, 60, 60, 20., 40.);
    REQUIRE_FALSE(image.empty());
    CHECK(alpha_at(image, 30, 30) == 255);
}
