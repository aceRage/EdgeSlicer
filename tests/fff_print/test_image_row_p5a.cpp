// ImageMap Phase 5a: the image row on the walls, reached the way the dialog reaches it, and
// correct where phase 4 was not.
//
// Spec: docs/superpowers/specs/2026-10-16-imagemap-phase5-completion.md, criteria B-G.
//
// Like test_image_row_walls.cpp these slice to G-code and read the moves back, because the wall
// split happens at G-code emission time and the G-code is the only place its result exists.
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

// Red, green, blue: the dither's nearest-colour choice for a saturated band is unambiguous.
const std::vector<std::string> kColours = {"#FF0000", "#00FF00", "#0000FF"};

std::vector<uint8_t> read_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

DynamicPrintConfig p5a_config(bool prime_tower)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(kColours.size()));
    config.set_num_filaments(unsigned(kColours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(kColours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(kColours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value    = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent  = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value    = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent  = false;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->percent = false;
    // A tool changer, so GCodeWriter emits T<n> (see test_image_row_walls.cpp).
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = prime_tower;
    if (prime_tower) {
        // Well clear of the part (which sits near the origin) and inside the default 200 mm bed.
        config.option<ConfigOptionFloats>("wipe_tower_x")->values    = {140.};
        config.option<ConfigOptionFloats>("wipe_tower_y")->values    = {140.};
        config.option<ConfigOptionFloat>("prime_tower_width")->value = 35.;
    }
    return config;
}

ModelVolume *make_part(Model &model, TriangleMesh mesh, const char *name)
{
    ModelObject *object = model.add_object();
    object->name        = name;
    ModelVolume *volume = object->add_volume(std::move(mesh));
    volume->name        = name;
    object->add_instance();
    object->ensure_on_bed();
    return volume;
}

// The Plater::apply_image_fill() row half, phase 5a: one ImageWeighted row over `ids`, with the
// surfaces in its params, bound to wall_filament always and to solid_infill_filament for the top.
int bind_row(MixedFilamentManager &mgr, ModelVolume &volume, ImageFillParams params, int surfaces,
             const std::vector<int> &ids = {1, 2, 3})
{
    params.surfaces = surfaces;
    mgr.add_custom_filament(unsigned(ids.front()), unsigned(ids.size() > 1 ? ids[1] : ids.front()), 50, kColours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &row         = mgr.mixed_filaments().back();
    row.enabled                = true;
    row.distribution_mode      = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(
        std::vector<unsigned int>(ids.begin(), ids.end()));
    row.image_fill_ref = MixedFilamentManager::encode_image_fill_ref(params.to_string());
    const unsigned int virtual_id = mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kColours.size());
    REQUIRE(virtual_id != 0);
    volume.config.set_key_value("wall_filament", new ConfigOptionInt(int(virtual_id)));
    if ((surfaces & IMAGE_FILL_SURFACE_TOP) != 0)
        volume.config.set_key_value("solid_infill_filament", new ConfigOptionInt(int(virtual_id)));
    return int(virtual_id);
}

void slice_to_result(Model &model, const DynamicPrintConfig &config, GCodeProcessorResult &result)
{
    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();
    const boost::filesystem::path out = Slic3r::Test::scratch_path(".gcode");
    print.export_gcode(out.string(), &result, nullptr);
    boost::nowide::remove(out.string().c_str());
}

// Distinct Z values at which `role` is extruded, ascending.
std::vector<float> role_zs(const GCodeProcessorResult &result, ExtrusionRole role)
{
    std::vector<float> zs;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == role) {
            bool seen = false;
            for (float z : zs)
                if (std::abs(z - m.position.z()) < 1e-4f) { seen = true; break; }
            if (!seen)
                zs.push_back(m.position.z());
        }
    std::sort(zs.begin(), zs.end());
    return zs;
}

std::set<int> extruders_at(const GCodeProcessorResult &result, float z, ExtrusionRole role)
{
    std::set<int> out;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == role && std::abs(m.position.z() - z) < 1e-4f)
            out.insert(int(m.extruder_id));
    return out;
}

std::string str(const std::set<int> &s)
{
    std::string out;
    for (int v : s)
        out += std::to_string(v) + " ";
    return out;
}

ImageFillParams box_params(Model &model, const char *fixture)
{
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_fixture(fixture));
    p.projection = ImageFillProjection::Box;
    p.allowed    = {1, 2, 3};
    return p;
}

} // namespace

// ================================================================================================
// B. Side walls through the dialog path: facet painting AND the row. Before 5a, every painted
//    region's wall_filament was overwritten with the painted physical id (PrintApply.cpp), so the
//    row never reached the walls the painting covers - which, for a box projection, is all of them.
// ================================================================================================
TEST_CASE("Image Row 5a: side walls survive the dialog's facet painting", "[imagefill][ImageRow][p5a]")
{
    Model                 model;
    ModelVolume          *volume = make_part(model, make_cube(30., 30., 30.), "p5a cube");
    const ImageFillParams p      = box_params(model, "bands3.png");

    // Step 1 of Plater::apply_image_fill(): the facet painting.
    const std::vector<std::array<float, 3>> colors = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};
    const ImageFillResult res = image_fill_apply(*volume, p, model.image_assets, colors, {1, 2, 3});
    REQUIRE(res.ok);
    REQUIRE(volume->is_mm_painted());

    // Step 2: the row, bound to the walls.
    MixedFilamentManager mgr;
    const int virtual_id = bind_row(mgr, *volume, p, IMAGE_FILL_SURFACE_WALLS);

    DynamicPrintConfig config = p5a_config(false);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);
    const std::vector<float> zs = role_zs(result, erExternalPerimeter);
    REQUIRE(zs.size() > 5);
    const float          mid = zs[zs.size() / 2];
    const std::set<int>  ext = extruders_at(result, mid, erExternalPerimeter);
    INFO("mid z=" << mid << " outer wall extruders: " << str(ext) << " virtual id " << virtual_id);
    // More than the one colour a single painted facet region would give its walls.
    CHECK(ext.size() >= 2);
    for (int e : ext)
        CHECK(e < int(kColours.size()));
}

// ================================================================================================
// C. With the prime tower ON, every tool a layer changes to is one that layer actually prints
//    with. Phase 4 registered the row's whole candidate list on every wall layer, and every id in
//    a layer's tool list costs a tool change and a tower purge whether anything prints with it.
// ================================================================================================
TEST_CASE("Image Row 5a: a wall layer only changes to tools it prints with", "[imagefill][ImageRow][p5a][tower]")
{
    Model                 model;
    ModelVolume          *volume = make_part(model, make_cube(30., 30., 30.), "p5a tower cube");
    const ImageFillParams p      = box_params(model, "bands3.png");
    MixedFilamentManager  mgr;
    bind_row(mgr, *volume, p, IMAGE_FILL_SURFACE_WALLS);

    DynamicPrintConfig config = p5a_config(true);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);

    // For each tool change, the layer it belongs to is the Z of the next extrusion; the tools a
    // layer prints the OBJECT with are every non-tower extrusion at that Z.
    std::map<int, std::set<int>> changed_to;   // layer z (um) -> tools changed to
    std::map<int, std::set<int>> printed_with; // layer z (um) -> tools with object extrusions
    int pending = -1;
    for (const auto &m : result.moves) {
        if (m.type == EMoveType::Tool_change) {
            pending = int(m.extruder_id);
            continue;
        }
        if (m.type != EMoveType::Extrude)
            continue;
        const int zk = int(std::lround(m.position.z() * 1000.f));
        if (pending >= 0) {
            changed_to[zk].insert(pending);
            pending = -1;
        }
        if (m.extrusion_role != erWipeTower)
            printed_with[zk].insert(int(m.extruder_id));
    }
    REQUIRE_FALSE(changed_to.empty());
    int checked = 0;
    for (const auto &[zk, tools] : changed_to) {
        if (zk < 600) // the first layers carry the skirt/brim and the tower's own start
            continue;
        const std::set<int> &used = printed_with[zk];
        INFO("layer z=" << zk / 1000.f << " changed to: " << str(tools) << " printed with: " << str(used));
        for (int t : tools)
            CHECK(used.count(t) == 1);
        ++checked;
    }
    CHECK(checked > 5);
}

// ================================================================================================
// D. The faces rule on the walls, and the part's own filament where the image does not land.
//    A flat Z picture bound to the walls must not be smeared down the sides: every side of the
//    cube is parallel to the projection, so every wall layer below the top prints the part's own
//    filament (filament 1, the cube's default) - and so do the inner walls, which never are the
//    row's per-layer cycle colour.
// ================================================================================================
TEST_CASE("Image Row 5a: a flat Z picture leaves the side walls in the part's own filament",
          "[imagefill][ImageRow][p5a][faces]")
{
    Model           model;
    ModelVolume    *volume = make_part(model, make_cube(30., 30., 30.), "p5a flat cube");
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_fixture("bands3.png"));
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};
    MixedFilamentManager mgr;
    bind_row(mgr, *volume, p, IMAGE_FILL_SURFACE_WALLS);
    // The part's own filament is 2 (green), so "the base" is distinguishable from candidate 1.
    volume->config.set_key_value("extruder", new ConfigOptionInt(2));
    volume->config.set_key_value("wall_loops", new ConfigOptionInt(3));

    DynamicPrintConfig config = p5a_config(false);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);
    const std::vector<float> zs = role_zs(result, erExternalPerimeter);
    REQUIRE(zs.size() > 10);
    for (size_t i = 2; i + 3 < zs.size(); i += 7) {
        const std::set<int> outer = extruders_at(result, zs[i], erExternalPerimeter);
        const std::set<int> inner = extruders_at(result, zs[i], erPerimeter);
        INFO("z=" << zs[i] << " outer: " << str(outer) << " inner: " << str(inner));
        CHECK(outer == std::set<int>{1});   // 0-based extruder 1 = filament 2
        CHECK(inner == std::set<int>{1});
    }
}

// ================================================================================================
// E. No base-colour frame round a top picture: with "Top surfaces", the top layer's outer wall is
//    seen from above and takes the picture's colours; the walls below keep the part's filament.
// ================================================================================================
TEST_CASE("Image Row 5a: the top edge of the outer wall carries the top picture", "[imagefill][ImageRow][p5a][ring]")
{
    Model           model;
    ModelVolume    *volume = make_part(model, make_cube(50., 50., 3.), "p5a plaque");
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_fixture("ramp_kw.png"));
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};
    MixedFilamentManager mgr;
    bind_row(mgr, *volume, p, IMAGE_FILL_SURFACE_TOP);

    DynamicPrintConfig config = p5a_config(false);
    // A black / grey / white set, so the ramp walks all three.
    config.option<ConfigOptionStrings>("filament_colour")->values = {"#000000", "#808080", "#FFFFFF"};
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_to_result(model, config, result);
    const std::vector<float> zs = role_zs(result, erExternalPerimeter);
    REQUIRE(zs.size() > 4);
    const std::set<int> top_ring = extruders_at(result, zs.back(), erExternalPerimeter);
    const std::set<int> below    = extruders_at(result, zs[zs.size() / 2], erExternalPerimeter);
    INFO("top layer outer wall: " << str(top_ring) << " mid layer outer wall: " << str(below));
    CHECK(top_ring.size() >= 2);
    CHECK(below == std::set<int>{0});   // the part's own filament, filament 1
}

// ================================================================================================
// G. A project whose picture is referenced only by a row bound to the walls saves and reloads with
//    the asset, the row and the binding, and slices the same walls again.
// ================================================================================================
TEST_CASE("Image Row 5a: a wall-bound row and its picture survive save and reload", "[imagefill][ImageRow][p5a][3mf]")
{
    Model                 model;
    ModelVolume          *volume = make_part(model, make_cube(30., 30., 30.), "p5a 3mf cube");
    const ImageFillParams p      = box_params(model, "bands3.png");
    MixedFilamentManager  mgr;
    const int             virtual_id = bind_row(mgr, *volume, p, IMAGE_FILL_SURFACE_WALLS);
    REQUIRE(model.image_assets.size() == 1);

    DynamicPrintConfig config = p5a_config(false);
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult before;
    slice_to_result(model, config, before);

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "image_row_p5a_walls.3mf").string();

    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &model;
    store_params.config   = &config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    const bool stored = store_bbs_3mf(store_params);

    Model                     loaded_model;
    DynamicPrintConfig        loaded_config;
    ConfigSubstitutionContext ctxt{ForwardCompatibilitySubstitutionRule::EnableSilent};
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &loaded_config, &ctxt, &loaded_model, &plate_data,
                                               &project_presets, &is_bbl_3mf, &file_version, nullptr,
                                               LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                   LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
    release_PlateData_list(plate_data);
    boost::filesystem::remove(test_file);
    REQUIRE(stored);
    REQUIRE(loaded);
    REQUIRE(loaded_model.objects.size() == 1);

    // The picture came back under its hash, though only the row names it.
    CHECK(loaded_model.image_assets.find(p.asset) != nullptr);
    // The binding came back.
    const ModelVolume *lv = loaded_model.objects[0]->volumes[0];
    const ConfigOption *wall = lv->config.option("wall_filament");
    REQUIRE(wall != nullptr);
    CHECK(wall->getInt() == virtual_id);
    // The row came back, with its surfaces.
    const ConfigOptionString *defs = loaded_config.option<ConfigOptionString>("mixed_filament_definitions");
    REQUIRE(defs != nullptr);
    MixedFilamentManager back;
    back.load_custom_entries(defs->value, kColours);
    const MixedFilament *row = back.mixed_filament_from_id(unsigned(virtual_id), kColours.size());
    REQUIRE(row != nullptr);
    ImageFillParams back_params;
    REQUIRE(ImageFillParams::from_string(MixedFilamentManager::decode_image_fill_ref(row->image_fill_ref), back_params));
    CHECK(back_params.surfaces == IMAGE_FILL_SURFACE_WALLS);
    CHECK(back_params.asset == p.asset);

    // And the reloaded project slices the same walls.
    DynamicPrintConfig reslice = p5a_config(false);
    reslice.set_key_value("mixed_filament_definitions", new ConfigOptionString(defs->value));
    GCodeProcessorResult after;
    slice_to_result(loaded_model, reslice, after);
    const std::vector<float> zb = role_zs(before, erExternalPerimeter);
    const std::vector<float> za = role_zs(after, erExternalPerimeter);
    REQUIRE(zb.size() == za.size());
    const float mid = zb[zb.size() / 2];
    CHECK(extruders_at(before, mid, erExternalPerimeter) == extruders_at(after, mid, erExternalPerimeter));
}
