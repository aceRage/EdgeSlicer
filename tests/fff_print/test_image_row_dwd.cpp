// Dual wall dithering on image-row walls (IMAGE_FILL_WALL_DWD): the outer wall and the first inner
// wall are cut into runs whose filament comes from a solved pair of layer sequences.
// Design: tests/design_dual_wall_dithering.md, section 2.1 (P3).
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/DualWallDither.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

// Cyan, magenta, yellow and a white backing: the documented recipe.
const std::vector<std::string> kDwdColours = {"#0099E6", "#E61A80", "#FFE61A", "#FFFFFF"};

std::vector<uint8_t> read_dwd_fixture(const std::string &name)
{
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/" + name;
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

DynamicPrintConfig dwd_test_config()
{
    const size_t       n      = kDwdColours.size();
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(n));
    config.set_num_filaments(unsigned(n));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(n, 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kDwdColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(n, 0.4);
    config.option<ConfigOptionFloats>("filament_transmission_distance")->values = std::vector<double>(n, 6.);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.set_key_value("wall_loops", new ConfigOptionInt(3));
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
    // A tool changer, so T<n> is emitted; no prime tower, so the per-layer bookkeeping is the part's own.
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value             = false;
    return config;
}

// A 30 mm cube whose own filament is white (4), with an image row of C, M, Y on its walls.
// `scale` shrinks the mesh and scales the instance back up, so the print is 30 mm either way.
int make_dwd_cube(Model &model, MixedFilamentManager &mgr, int wall_method, ImageFillParams p, double scale = 1.)
{
    ModelObject *object = model.add_object();
    object->name        = "dwd cube";
    ModelVolume *volume = object->add_volume(make_cube(30. / scale, 30. / scale, 30. / scale));
    volume->name        = "cube";
    object->add_instance();
    object->instances.front()->set_scaling_factor(Vec3d(scale, scale, scale));
    object->ensure_on_bed();
    volume->config.set_key_value("extruder", new ConfigOptionInt(4));

    p.allowed     = {1, 2, 3};
    p.surfaces    = IMAGE_FILL_SURFACE_WALLS;
    p.wall_method = wall_method;

    const std::vector<unsigned int> ids = {1, 2, 3};
    mgr.add_custom_filament(1, 2, 50, kDwdColours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &row         = mgr.mixed_filaments().back();
    row.enabled                = true;
    row.distribution_mode      = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(ids);
    row.image_fill_ref         = MixedFilamentManager::encode_image_fill_ref(p.to_string());
    REQUIRE_FALSE(row.image_fill_ref.empty());
    const unsigned int virtual_id = mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kDwdColours.size());
    REQUIRE(virtual_id != 0);
    volume->config.set_key_value("wall_filament", new ConfigOptionInt(int(virtual_id)));
    return int(virtual_id);
}

void slice_dwd(Model &model, const DynamicPrintConfig &config, GCodeProcessorResult &result)
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

// Per layer Z: how many extruding moves of `role` each extruder made, and the layer's tool changes.
struct DwdLayer
{
    std::map<int, size_t> outer;   // erExternalPerimeter
    std::map<int, size_t> inner;   // erPerimeter
    int                   tool_changes = 0;
};

std::map<int, DwdLayer> layers_of(const GCodeProcessorResult &result)
{
    std::map<int, DwdLayer> layers;   // key: Z in microns
    for (const auto &m : result.moves) {
        const int z = int(std::lround(m.position.z() * 1000.f));
        if (m.type == EMoveType::Tool_change) {
            ++layers[z].tool_changes;
            continue;
        }
        if (m.type != EMoveType::Extrude)
            continue;
        if (m.extrusion_role == erExternalPerimeter)
            ++layers[z].outer[int(m.extruder_id)];
        else if (m.extrusion_role == erPerimeter)
            ++layers[z].inner[int(m.extruder_id)];
    }
    return layers;
}

// The layers of the middle of the cube that carry walls: clear of the bottom and the top skins.
std::vector<DwdLayer> middle_layers(const GCodeProcessorResult &result, size_t count)
{
    std::vector<DwdLayer> with_walls;
    for (const auto &kv : layers_of(result))
        if (!kv.second.outer.empty())
            with_walls.push_back(kv.second);
    REQUIRE(with_walls.size() > count + 20);
    const size_t start = with_walls.size() / 2 - count / 2;
    return std::vector<DwdLayer>(with_walls.begin() + start, with_walls.begin() + start + count);
}

} // namespace

TEST_CASE("DwD walls: the wall method survives the params string", "[dwd][imagefill]")
{
    ImageFillParams p;
    p.wall_method = IMAGE_FILL_WALL_DWD;
    ImageFillParams back;
    REQUIRE(ImageFillParams::from_string(p.to_string(), back));
    CHECK(back.wall_method == IMAGE_FILL_WALL_DWD);
}

// The pair the test asks for: the one matched to the colour of an outer wall that alternates two
// filaments. Matching is outer first, so that colour comes back with that outer wall.
DwdPair isolated_alternating_pair()
{
    std::vector<DwdFilament> filaments;
    for (const std::string &hex : kDwdColours)
        filaments.push_back({MixedFilamentManager::hex_to_srgb01(hex), 6.});
    DwdModelParams model;
    model.outer_width_mm = 0.45;
    model.inner_width_mm = 0.45;
    DwdTableParams params;
    params.sequence_filaments = 3;
    const std::vector<DwdPair> table = dwd_build_table(filaments, 3, model, params);
    REQUIRE(table.size() > 10);

    int    best = -1;
    double best_gap = 0.;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].outer.size() != 2)
            continue;
        double gap = 1e9;
        for (size_t j = 0; j < table.size(); ++j)
            if (j != i) {
                double d = 0.;
                for (int c = 0; c < 3; ++c)
                    d += std::pow(double(table[i].oklab[c]) - double(table[j].oklab[c]), 2);
                gap = std::min(gap, std::sqrt(d));
            }
        if (gap > best_gap) {
            best_gap = gap;
            best     = int(i);
        }
    }
    REQUIRE(best >= 0);
    const int match = dwd_nearest(table, table[size_t(best)].outer_srgb);
    REQUIRE(match >= 0);
    REQUIRE(table[size_t(match)].outer == table[size_t(best)].outer);
    return table[size_t(match)];
}

TEST_CASE("DwD walls: a pair's sequences reach the two walls layer by layer", "[dwd][imagefill][walls]")
{
    const DwdPair pair = isolated_alternating_pair();
    REQUIRE(pair.outer.size() == 2);
    REQUIRE(pair.outer[0] != pair.outer[1]);

    // A "gradient" of one colour: every point of every wall asks for exactly this pair.
    ImageFillParams p;
    p.gradient.enabled = true;
    p.gradient.stop_a  = pair.outer_srgb;
    p.gradient.stop_b  = pair.outer_srgb;
    p.projection       = ImageFillProjection::Box;

    Model                model;
    MixedFilamentManager mgr;
    const int            virtual_id = make_dwd_cube(model, mgr, IMAGE_FILL_WALL_DWD, p);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const std::vector<DwdLayer> mid = middle_layers(result, 8);

    std::string dump;
    for (const DwdLayer &layer : mid) {
        dump += "outer";
        for (const auto &kv : layer.outer)
            dump += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
        dump += " inner";
        for (const auto &kv : layer.inner)
            dump += " " + std::to_string(kv.first) + ":" + std::to_string(kv.second);
        dump += " | ";
    }
    INFO("virtual id " << virtual_id << "; " << dump);

    std::set<int> inner_allowed(pair.inner.begin(), pair.inner.end());
    inner_allowed.insert(3);   // the wall behind the first inner wall is the part's own white
    for (size_t i = 0; i < mid.size(); ++i) {
        const DwdLayer &layer = mid[i];
        // The outer wall is one filament per layer, one of the pair's two...
        REQUIRE(layer.outer.size() == 1);
        const int outer = layer.outer.begin()->first;
        const bool of_pair = outer == int(pair.outer[0]) || outer == int(pair.outer[1]);
        CHECK(of_pair);
        // ...and the other one on the next layer.
        if (i > 0)
            CHECK(outer != mid[i - 1].outer.begin()->first);

        // The inner walls print the pair's inner sequence and the backing, nothing else.
        for (const auto &kv : layer.inner)
            CHECK(inner_allowed.count(kv.first) == 1);
        CHECK(layer.inner.count(3) == 1);

        // Tool changes are bounded by the filament count, not by the picture.
        CHECK(layer.tool_changes <= 2 * (int(kDwdColours.size()) - 1));
    }
}

TEST_CASE("DwD walls: plain colour runs are not affected", "[dwd][imagefill][walls]")
{
    Model                model;
    MixedFilamentManager mgr;
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_dwd_fixture("bands3.png"));
    p.projection = ImageFillProjection::Box;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_RUNS, p);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const std::vector<DwdLayer> mid = middle_layers(result, 8);

    // Colour runs put the picture's nearest filament on the wall, the same on every layer.
    for (size_t i = 1; i < mid.size(); ++i) {
        INFO("middle layer " << i);
        std::set<int> a, b;
        for (const auto &kv : mid[i - 1].outer)
            a.insert(kv.first);
        for (const auto &kv : mid[i].outer)
            b.insert(kv.first);
        CHECK(a == b);
    }
}

// ---------------------------------------------------------------------------------------------
// P4: a textured model - the walls read the part's own texture through its own UVs
// ---------------------------------------------------------------------------------------------

namespace {

// Gives the cube a 3 x 1 texture of the first three filament colours and UVs that put the first
// on the +X side, the second on the -X side and the third on everything else.
void texture_cube_by_side(Model &model)
{
    ModelVolume                *volume = model.objects.front()->volumes.front();
    const indexed_triangle_set &mesh   = volume->mesh().its;

    std::vector<uint8_t> rgb;
    for (size_t i = 0; i < 3; ++i) {
        const std::array<float, 3> c = MixedFilamentManager::hex_to_srgb01(kDwdColours[i]);
        for (float v : c)
            rgb.push_back(uint8_t(std::lround(v * 255.f)));
    }
    ImageFillUVs uvs(mesh.indices.size());
    for (size_t f = 0; f < uvs.size(); ++f) {
        const Vec3f &a = mesh.vertices[mesh.indices[f](0)], &b = mesh.vertices[mesh.indices[f](1)],
                    &c = mesh.vertices[mesh.indices[f](2)];
        const Vec3f n    = (b - a).cross(c - a).normalized();
        const int   cell = n.x() > 0.5f ? 0 : n.x() < -0.5f ? 1 : 2;
        for (size_t k = 0; k < 3; ++k)
            uvs[f][k] = Vec2f((float(cell) + 0.5f) / 3.f, 0.5f);
    }
    REQUIRE(image_fill_set_volume_texture(*volume, model.image_assets, image_fill_encode_png(3, 1, rgb.data()), uvs));
}

// The extruders the outer wall used on the +X side, the -X side and the two Y sides, in the middle
// of the cube's height and clear of the corners.
struct SideExtruders
{
    std::map<int, size_t> plus_x, minus_x, y_sides;
};

SideExtruders outer_wall_by_side(const GCodeProcessorResult &result)
{
    struct Piece { Vec3f mid; int extruder; };
    std::vector<Piece> pieces;
    BoundingBoxf3      box;
    for (size_t i = 1; i < result.moves.size(); ++i) {
        const auto &m = result.moves[i];
        if (m.type != EMoveType::Extrude || m.extrusion_role != erExternalPerimeter)
            continue;
        const Vec3f mid = 0.5f * (result.moves[i - 1].position + m.position);
        pieces.push_back({mid, int(m.extruder_id)});
        box.merge(mid.cast<double>());
    }
    SideExtruders out;
    if (pieces.empty())
        return out;
    const Vec3d  centre = box.center();
    const double half   = 0.5 * box.size().x();
    for (const Piece &p : pieces) {
        if (p.mid.z() < 3.f || p.mid.z() > 27.f)
            continue;
        const double dx = double(p.mid.x()) - centre.x(), dy = double(p.mid.y()) - centre.y();
        if (std::abs(dy) < half - 4. && dx > half - 0.5)
            ++out.plus_x[p.extruder];
        else if (std::abs(dy) < half - 4. && dx < -(half - 0.5))
            ++out.minus_x[p.extruder];
        else if (std::abs(dx) < half - 4. && std::abs(dy) > half - 0.5)
            ++out.y_sides[p.extruder];
    }
    return out;
}

} // namespace

TEST_CASE("Mesh UV walls: each side of a textured cube prints its own texture colour", "[dwd][imagefill][walls][meshuv]")
{
    ImageFillParams p;
    p.projection = ImageFillProjection::MeshUV;
    p.faces      = ImageFillFaces::All;

    Model                model;
    MixedFilamentManager mgr;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_RUNS, p);
    texture_cube_by_side(model);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const SideExtruders sides = outer_wall_by_side(result);

    // Filaments 1, 2, 3 are extruders 0, 1, 2: one colour per side, and nothing else there.
    REQUIRE(sides.plus_x.size() == 1);
    CHECK(sides.plus_x.begin()->first == 0);
    REQUIRE(sides.minus_x.size() == 1);
    CHECK(sides.minus_x.begin()->first == 1);
    REQUIRE(sides.y_sides.size() == 1);
    CHECK(sides.y_sides.begin()->first == 2);
}

// A scaled part: the samplers measure along the wall in mesh space, the runs are cut along the wall
// as printed. Mixing the two put every colour change at the wrong place on a scaled model.
TEST_CASE("Mesh UV walls: a scaled textured cube still prints each side's own colour", "[dwd][imagefill][walls][meshuv]")
{
    ImageFillParams p;
    p.projection = ImageFillProjection::MeshUV;
    p.faces      = ImageFillFaces::All;

    Model                model;
    MixedFilamentManager mgr;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_RUNS, p, 3.);
    texture_cube_by_side(model);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const SideExtruders sides = outer_wall_by_side(result);

    REQUIRE(sides.plus_x.size() == 1);
    CHECK(sides.plus_x.begin()->first == 0);
    REQUIRE(sides.minus_x.size() == 1);
    CHECK(sides.minus_x.begin()->first == 1);
    REQUIRE(sides.y_sides.size() == 1);
    CHECK(sides.y_sides.begin()->first == 2);
}

TEST_CASE("Mesh UV walls: a part whose UVs no longer fit keeps its own filament", "[dwd][imagefill][walls][meshuv]")
{
    ImageFillParams p;
    p.projection = ImageFillProjection::MeshUV;
    p.faces      = ImageFillFaces::All;

    Model                model;
    MixedFilamentManager mgr;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_RUNS, p);
    // No texture was ever stored on this part: the row has nothing to read.

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const SideExtruders sides = outer_wall_by_side(result);

    // The part's own filament is 4 (extruder 3), on every side.
    REQUIRE(sides.plus_x.size() == 1);
    CHECK(sides.plus_x.begin()->first == 3);
    REQUIRE(sides.minus_x.size() == 1);
    CHECK(sides.minus_x.begin()->first == 3);
    REQUIRE(sides.y_sides.size() == 1);
    CHECK(sides.y_sides.begin()->first == 3);
}

TEST_CASE("Mesh UV walls: dual wall dithering reads the texture too", "[dwd][imagefill][walls][meshuv]")
{
    ImageFillParams p;
    p.projection = ImageFillProjection::MeshUV;
    p.faces      = ImageFillFaces::All;

    Model                model;
    MixedFilamentManager mgr;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_DWD, p);
    texture_cube_by_side(model);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const SideExtruders sides = outer_wall_by_side(result);

    // A pure filament colour is its own nearest pair's outer wall, so each side's outer wall still
    // carries that side's filament - through the pair table this time.
    REQUIRE_FALSE(sides.plus_x.empty());
    REQUIRE_FALSE(sides.minus_x.empty());
    CHECK(sides.plus_x.count(0) == 1);
    CHECK(sides.minus_x.count(1) == 1);
    CHECK(sides.y_sides.count(2) == 1);
    // The backing never shows on the outer wall of a textured side.
    CHECK(sides.plus_x.count(3) == 0);
    CHECK(sides.minus_x.count(3) == 0);
}

// The colour import dialog's per-colour choice: a colour group set to one filament prints that
// filament on the dithered walls; a group set to Mix is dithered as before.
TEST_CASE("DwD walls: per-colour choices survive the params string", "[dwd][imagefill]")
{
    ImageFillParams p;
    p.color_choices = {{{1.f, 0.f, 0.f}, 3}, {{0.f, 0.6f, 0.9f}, 0}};
    ImageFillParams back;
    REQUIRE(ImageFillParams::from_string(p.to_string(), back));
    REQUIRE(back.color_choices.size() == 2);
    CHECK(back.color_choices[0].filament == 3);
    CHECK(back.color_choices[1].filament == 0);
    CHECK_THAT(back.color_choices[1].color[1], Catch::Matchers::WithinAbs(0.6, 0.003));

    // The nearest group decides; no groups means mix.
    CHECK(image_fill_color_choice(back.color_choices, {0.9f, 0.1f, 0.1f}) == 3);
    CHECK(image_fill_color_choice(back.color_choices, {0.1f, 0.5f, 0.8f}) == 0);
    CHECK(image_fill_color_choice({}, {0.9f, 0.1f, 0.1f}) == 0);
    CHECK(ImageFillParams().to_string().find("cl=") == std::string::npos);
}

TEST_CASE("Mesh UV walls: a colour set to one filament prints it instead of the mix", "[dwd][imagefill][walls][meshuv]")
{
    ImageFillParams p;
    p.projection = ImageFillProjection::MeshUV;
    p.faces      = ImageFillFaces::All;
    // The +X side's colour (filament 1's) goes to filament 3 on purpose, so the choice shows and
    // not the colour match; the -X side's colour stays a mix.
    p.color_choices = {{MixedFilamentManager::hex_to_srgb01(kDwdColours[0]), 3},
                       {MixedFilamentManager::hex_to_srgb01(kDwdColours[1]), 0},
                       {MixedFilamentManager::hex_to_srgb01(kDwdColours[2]), 0}};

    Model                model;
    MixedFilamentManager mgr;
    make_dwd_cube(model, mgr, IMAGE_FILL_WALL_DWD, p);
    texture_cube_by_side(model);

    DynamicPrintConfig config = dwd_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    GCodeProcessorResult result;
    slice_dwd(model, config, result);
    const SideExtruders sides = outer_wall_by_side(result);

    REQUIRE(sides.plus_x.size() == 1);
    CHECK(sides.plus_x.begin()->first == 2);
    CHECK(sides.minus_x.count(1) == 1);
    CHECK(sides.y_sides.count(2) == 1);
}
