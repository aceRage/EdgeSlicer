// ImageMap Phase 5b: layer stripes - ImageMap's own side-wall method.
//
// Spec: docs/superpowers/specs/2026-10-16-imagemap-phase5-completion.md, criterion H.
//
// Every wall layer prints with ONE filament, taking the row's filaments in turn layer after layer,
// and the outer wall is wider where the picture wants that layer's colour and narrower where it
// does not. Measured on the G-code, like the other wall tests, because the modulation happens at
// G-code emission time.
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

#include "test_data.hpp"

using namespace Slic3r;

namespace {

const std::vector<std::string> kColours = {"#FF0000", "#00FF00", "#0000FF"};

DynamicPrintConfig stripe_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(kColours.size()));
    config.set_num_filaments(unsigned(kColours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(kColours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(kColours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value    = true;
    config.option<ConfigOptionBool>("enable_prime_tower")->value                = false;
    return config;
}

struct WallMove
{
    float x, y, z, width;
    int   extruder;
};

std::vector<WallMove> outer_wall_moves(const GCodeProcessorResult &result)
{
    std::vector<WallMove> out;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erExternalPerimeter)
            out.push_back(WallMove{m.position.x(), m.position.y(), m.position.z(), m.width, int(m.extruder_id)});
    return out;
}

} // namespace

TEST_CASE("Image Row 5b: layer stripes - one filament per wall layer, width follows the picture",
          "[imagefill][ImageRow][p5b][stripes]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "p5b stripes cube";
    ModelVolume *volume = object->add_volume(make_cube(30., 30., 30.));
    volume->name        = "cube";
    object->add_instance();
    object->ensure_on_bed();

    // Red at u = 0, green at u = 1, on every face of the box. Blue is allowed but never wanted.
    ImageFillParams p;
    p.gradient.enabled   = true;
    p.gradient.stop_a    = {1.f, 0.f, 0.f};
    p.gradient.stop_b    = {0.f, 1.f, 0.f};
    p.gradient.direction = 0;
    p.projection         = ImageFillProjection::Box;
    p.allowed            = {1, 2, 3};
    p.surfaces           = IMAGE_FILL_SURFACE_WALLS;
    p.wall_method        = IMAGE_FILL_WALL_STRIPES;
    p.stripe_depth_pct   = 40;

    MixedFilamentManager mgr;
    mgr.add_custom_filament(1, 2, 50, kColours);
    MixedFilament &row         = mgr.mixed_filaments().back();
    row.enabled                = true;
    row.distribution_mode      = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids({1, 2, 3});
    row.image_fill_ref         = MixedFilamentManager::encode_image_fill_ref(p.to_string());
    const unsigned int vid     = mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kColours.size());
    REQUIRE(vid != 0);
    volume->config.set_key_value("wall_filament", new ConfigOptionInt(int(vid)));

    DynamicPrintConfig config = stripe_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    print.process();
    GCodeProcessorResult         result;
    const boost::filesystem::path out = Slic3r::Test::scratch_path(".gcode");
    print.export_gcode(out.string(), &result, nullptr);
    boost::nowide::remove(out.string().c_str());

    const std::vector<WallMove> moves = outer_wall_moves(result);
    REQUIRE_FALSE(moves.empty());
    std::map<int, std::vector<WallMove>> by_layer;   // z in microns -> moves
    for (const WallMove &m : moves)
        by_layer[int(std::lround(m.z * 1000.f))].push_back(m);
    REQUIRE(by_layer.size() > 20);

    // Skip the first and last few layers (brim, top surface).
    std::vector<int> layer_tool;
    int              checked_red = 0, checked_green = 0, checked_blue = 0;
    size_t           li = 0;
    for (const auto &[zk, lm] : by_layer) {
        ++li;
        if (li < 4 || li + 4 > by_layer.size())
            continue;
        std::set<int> tools;
        float         ymin = 1e9f, xmin = 1e9f, xmax = -1e9f, wmin = 1e9f, wmax = -1e9f;
        for (const WallMove &m : lm) {
            tools.insert(m.extruder);
            ymin = std::min(ymin, m.y);
            wmin = std::min(wmin, m.width);
            wmax = std::max(wmax, m.width);
        }
        INFO("z=" << zk / 1000.f << " tools on the outer wall: " << tools.size() << " width " << wmin << ".." << wmax);
        // H.1: one filament for the whole wall layer.
        REQUIRE(tools.size() == 1);
        const int tool = *tools.begin();
        layer_tool.push_back(tool);

        // H.3: along the -Y face, u runs with +X: red wanted at small x, green at large x.
        std::vector<WallMove> face;
        for (const WallMove &m : lm)
            if (m.y < ymin + 0.6f) {
                face.push_back(m);
                xmin = std::min(xmin, m.x);
                xmax = std::max(xmax, m.x);
            }
        if (face.size() < 4 || xmax - xmin < 10.f)
            continue;
        double w_lo = 0., w_hi = 0.;
        int    n_lo = 0, n_hi = 0;
        for (const WallMove &m : face) {
            if (m.x < xmin + 0.25f * (xmax - xmin)) { w_lo += m.width; ++n_lo; }
            if (m.x > xmax - 0.25f * (xmax - xmin)) { w_hi += m.width; ++n_hi; }
        }
        if (n_lo == 0 || n_hi == 0)
            continue;
        w_lo /= n_lo;
        w_hi /= n_hi;
        INFO("tool " << tool << ": mean width at the red end " << w_lo << ", at the green end " << w_hi);
        if (tool == 0) {        // red layer: wide where red is wanted
            CHECK(w_lo > w_hi + 0.05);
            ++checked_red;
        } else if (tool == 1) { // green layer: wide where green is wanted
            CHECK(w_hi > w_lo + 0.05);
            ++checked_green;
        } else {                // blue is never wanted: narrow at both ends
            CHECK(w_lo < 0.45);
            CHECK(w_hi < 0.45);
            ++checked_blue;
        }
    }
    CHECK(checked_red > 2);
    CHECK(checked_green > 2);
    CHECK(checked_blue > 2);

    // H.2: the filaments take turns, layer after layer, in the row's order.
    REQUIRE(layer_tool.size() > 6);
    for (size_t i = 1; i < layer_tool.size(); ++i) {
        INFO("layer " << i << ": tool " << layer_tool[i - 1] << " then " << layer_tool[i]);
        CHECK(layer_tool[i] == (layer_tool[i - 1] + 1) % 3);
    }
}

// Not run by default: slicing time on a Benchy with a box-projected picture on its top and walls,
// against the same Benchy without it (spec criterion M). Run with "[.measure_p5]".
TEST_CASE("Image Row 5b: measure - Benchy slicing time with and without a picture", "[.measure_p5]")
{
    const std::string benchy = std::string(TEST_DATA_DIR) + "/../../resources/handy_models/3DBenchy.3mf";
    auto run = [&](int mode) -> double {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctx{ForwardCompatibilitySubstitutionRule::EnableSilent};
        Model model = Model::read_from_file(benchy, &cfg, &ctx, LoadStrategy::LoadModel | LoadStrategy::AddDefaultInstances);
        REQUIRE(model.objects.size() == 1);
        ModelVolume         *volume = model.objects[0]->volumes[0];
        MixedFilamentManager mgr;
        if (mode > 0) {
            ImageFillParams p;
            boost::nowide::ifstream in(std::string(TEST_DATA_DIR) + "/image_fill/quad_rgbw.png", std::ios::binary);
            p.asset       = model.image_assets.add(std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            p.projection  = ImageFillProjection::Box;
            p.allowed     = {1, 2, 3};
            p.surfaces    = IMAGE_FILL_SURFACE_TOP | IMAGE_FILL_SURFACE_WALLS;
            p.wall_method = mode == 2 ? IMAGE_FILL_WALL_STRIPES : IMAGE_FILL_WALL_RUNS;
            mgr.add_custom_filament(1, 2, 50, kColours);
            MixedFilament &row         = mgr.mixed_filaments().back();
            row.enabled                = true;
            row.distribution_mode      = int(MixedFilament::ImageWeighted);
            row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids({1, 2, 3});
            row.image_fill_ref         = MixedFilamentManager::encode_image_fill_ref(p.to_string());
            const unsigned int vid     = mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kColours.size());
            volume->config.set_key_value("wall_filament", new ConfigOptionInt(int(vid)));
            volume->config.set_key_value("solid_infill_filament", new ConfigOptionInt(int(vid)));
        }
        DynamicPrintConfig config = stripe_config();
        config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));
        const auto t0 = std::chrono::steady_clock::now();
        Print print;
        print.set_status_silent();
        print.apply(model, config);
        print.process();
        GCodeProcessorResult          result;
        const boost::filesystem::path out = Slic3r::Test::scratch_path(".gcode");
        print.export_gcode(out.string(), &result, nullptr);
        boost::nowide::remove(out.string().c_str());
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const double plain   = run(0);
    const double runs    = run(1);
    const double stripes = run(2);
    WARN("Benchy slice + export: plain " << plain << " s, picture with colour runs " << runs << " s ("
         << runs / plain << "x), picture with layer stripes " << stripes << " s (" << stripes / plain << "x)");
    CHECK(runs < plain * 3.);
}
