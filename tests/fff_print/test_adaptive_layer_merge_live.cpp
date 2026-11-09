// Mixed nozzle diameters, step 3 of the BambuStudio v02.09.01.70 AdaptiveLayerMerge port: the planner is live. On a
// plate whose used filaments print through nozzles of different diameters, a coarse nozzle stacks one slab per
// several layers of the fine nozzle: its region extrudes once on the slab's top layer at the slab's height, and
// nothing on the layers below it. A plate printed through one nozzle diameter is untouched.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "test_data.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// Two tools of the given diameters, one filament each. `layer` is both the first layer and the base layer height.
DynamicPrintConfig two_nozzles(const char *diameters, const char *layer, const char *min_heights, const char *max_heights)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.set_deserialize_strict({
        { "nozzle_diameter", diameters },
        { "filament_diameter", "1.75,1.75" },
        { "filament_map", "1,2" },
        { "single_extruder_multi_material", "0" },
        { "enable_prime_tower", "0" },
        { "enable_support", "0" },
        { "enable_arc_fitting", "0" },
        { "slow_down_for_layer_cooling", "0,0" },
        { "filament_max_volumetric_speed", "200,200" },
        { "nozzle_temperature", "210,210" },
        { "nozzle_temperature_initial_layer", "210,210" },
        { "wall_generator", "classic" },
        { "layer_height", layer },
        { "initial_layer_print_height", layer },
        { "min_layer_height", min_heights },
        { "max_layer_height", max_heights },
        { "brim_type", "no_brim" },
        { "skirt_loops", "0" },
        { "elefant_foot_compensation", "0" },
        { "sparse_infill_density", "15%" },
    });
    return config;
}

DynamicPrintConfig h3c_like_04_08() { return two_nozzles("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56"); }
DynamicPrintConfig h2d_like_02_04() { return two_nozzles("0.2,0.4", "0.1", "0.04,0.08", "0.14,0.28"); }
DynamicPrintConfig same_diameter_04() { return two_nozzles("0.4,0.4", "0.2", "0.08,0.08", "0.28,0.28"); }

// One cube per filament, side by side.
void add_cubes(Model &model, double size)
{
    for (int filament = 1; filament <= 2; ++filament) {
        ModelObject *object = model.add_object();
        object->name        = "cube-" + std::to_string(filament);
        object->add_volume(TriangleMesh(its_make_cube(size, size, size)));
        object->add_instance()->set_offset(Vec3d(40. + 40. * filament, 60., 0.));
        object->ensure_on_bed();
        object->volumes.front()->config.set("extruder", filament);
    }
}

const PrintObject *object_named(const Print &print, const std::string &name)
{
    for (const PrintObject *object : print.objects())
        if (object->model_object()->name == name)
            return object;
    return nullptr;
}

// The 0.8's ladder next to a 0.4 at the given window, as compute_adaptive_merge_groups() cuts it: layer 0 prints as
// sliced, the bands start on layer 1.
std::vector<AdaptiveLayerBand> build_adaptive_layer_ladder_for_test(const std::vector<double> &stack, AdaptiveLayerHeightRange range)
{
    std::vector<AdaptiveLayerBand> bands = build_adaptive_layer_bands(std::vector<double>(stack.begin() + 1, stack.end()),
                                                                      adaptive_layer_band_size(0.8, 0.4, stack.back(), range), range);
    for (AdaptiveLayerBand &band : bands) {
        ++band.bottom_idx;
        ++band.top_idx;
    }
    return bands;
}

// Heights of every extrusion path of a layer (walls, overhang walls, gap fill and infill). Bridge infill is left out:
// it prints with the bridging flow (a thick bridge is as tall as its nozzle), whatever the layer or slab height.
// Overhang walls are kept: on a slab they must carry the slab's height like every other wall.
bool is_bridge_infill(ExtrusionRole role) { return role == erBridgeInfill || role == erInternalBridgeInfill; }

void add_height(const ExtrusionPath &path, std::set<double> &out)
{
    if (! is_bridge_infill(path.role()))
        out.insert(std::round(path.height * 1000.) / 1000.);
}

void collect_heights(const ExtrusionEntity &entity, std::set<double> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_heights(*child, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        add_height(*path, out);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multi->paths)
            add_height(p, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            add_height(p, out);
    }
}

std::set<double> layer_heights_of(const Layer &layer)
{
    std::set<double> out;
    for (const LayerRegion *layerm : layer.regions()) {
        for (const ExtrusionEntity *e : layerm->perimeters.entities)
            collect_heights(*e, out);
        for (const ExtrusionEntity *e : layerm->fills.entities)
            collect_heights(*e, out);
        for (const ExtrusionEntity *e : layerm->thin_fills.entities)
            collect_heights(*e, out);
    }
    return out;
}

const AdaptiveMergePatch *group_top_patch(const Layer &layer)
{
    for (const LayerRegion *layerm : layer.regions())
        for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches)
            if (patch.group_top && ! patch.area().empty())
                return &patch;
    return nullptr;
}

struct GcodeLayer
{
    double              z { 0. };
    std::set<int>       tools;
    std::map<int, std::set<double>> heights; // tool -> ;HEIGHT: values of its extruding moves
};

std::vector<GcodeLayer> parse_layers(const std::string &gcode)
{
    std::vector<GcodeLayer> out;
    std::istringstream      in(gcode);
    std::string             line;
    int                     tool   = 0;
    double                  height = 0.;
    bool                    bridge = false;
    static const std::regex tool_re(R"(^T([0-9]+)\b)");
    static const std::regex z_re(R"(^;Z:([0-9.]+))");
    static const std::regex height_re(R"(^;HEIGHT:([0-9.]+))");
    static const std::regex e_re(R"(\bE[0-9.]+)");
    static const std::regex type_re(R"(^;TYPE:(.*)$)");
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        std::smatch m;
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            out.emplace_back();
            continue;
        }
        if (std::regex_search(line, m, z_re)) {
            if (! out.empty())
                out.back().z = std::stod(m[1].str());
            continue;
        }
        if (std::regex_search(line, m, height_re)) {
            height = std::stod(m[1].str());
            continue;
        }
        if (std::regex_search(line, m, type_re)) {
            // Bridge infill prints with the bridging flow, not the layer or slab height. Overhang walls are checked.
            bridge = m[1].str() == "Bridge" || m[1].str() == "Internal Bridge";
            continue;
        }
        if (std::regex_search(line, m, tool_re)) {
            tool = std::stoi(m[1].str());
            continue;
        }
        if (! out.empty() && line.compare(0, 2, "G1") == 0 && std::regex_search(line, e_re) &&
            line.find(" X") != std::string::npos) {
            out.back().tools.insert(tool);
            if (! bridge)
                out.back().heights[tool].insert(std::round(height * 1000.) / 1000.);
        }
    }
    return out;
}

struct Expectation
{
    double base;      // base layer height
    double slab;      // the coarse nozzle's slab height
    size_t per_slab;  // fine layers per coarse slab
};

// Slices the two-cube plate, then checks the plan, the extrusions, the tool ordering and the G-code.
void check_mixed_plate(const DynamicPrintConfig &config, double cube, const Expectation &expect)
{
    Model model;
    add_cubes(model, cube);
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.uses_multiple_nozzle_diameters());
    GCodeProcessorResult result;
    const std::string    gcode = Test::gcode(print, result);
    REQUIRE(! gcode.empty());

    const PrintObject *fine   = object_named(print, "cube-1");
    const PrintObject *coarse = object_named(print, "cube-2");
    REQUIRE(fine != nullptr);
    REQUIRE(coarse != nullptr);
    REQUIRE(fine->layer_count() == coarse->layer_count());
    const size_t layers = coarse->layer_count();
    REQUIRE(layers > 2 * expect.per_slab);

    // The fine object is its own reference: no patches, every layer prints at the base height.
    for (const Layer *layer : fine->layers()) {
        for (const LayerRegion *layerm : layer->regions())
            CHECK(layerm->adaptive_merge_patches.empty());
        CHECK(layer->has_extrusions());
        const std::set<double> h = layer_heights_of(*layer);
        CHECK(h == std::set<double>{ std::round(layer->height * 1000.) / 1000. });
    }

    // The coarse object: layer 0 is the bed contact and prints as sliced; above it the layers stack into slabs.
    const Layer *first = coarse->layers().front();
    CHECK(first->has_extrusions());
    CHECK(group_top_patch(*first) == nullptr);
    CHECK(layer_heights_of(*first) == std::set<double>{ std::round(first->height * 1000.) / 1000. });

    size_t                slab_tops = 0, full_slabs = 0;
    std::set<double>         coarse_print_z;
    std::map<double, double> coarse_slab_height; // rounded print_z -> slab height
    coarse_print_z.insert(std::round(first->print_z * 1000.) / 1000.);
    coarse_slab_height[std::round(first->print_z * 1000.) / 1000.] = first->height;
    for (size_t idx = 1; idx < layers; ++idx) {
        const Layer *layer = coarse->layers()[idx];
        CAPTURE(idx);
        const AdaptiveMergePatch *top = group_top_patch(*layer);
        if (top == nullptr) {
            // Swallowed by the slab above: nothing printed, and it is reported as merged out, not as a gap.
            CHECK_FALSE(layer->has_extrusions());
            CHECK(layer->is_adaptive_merged_out());
            continue;
        }
        ++slab_tops;
        coarse_print_z.insert(std::round(layer->print_z * 1000.) / 1000.);
        coarse_slab_height[std::round(layer->print_z * 1000.) / 1000.] = top->merged_height;
        CHECK(top->top_idx == idx);
        CHECK(top->bottom_idx >= 1);
        CHECK(layer->has_extrusions());
        CHECK_THAT(layer->effective_print_height(), WithinAbs(top->merged_height, 1e-9));
        // Every path of the slab is laid at the slab's height.
        CHECK(layer_heights_of(*layer) == std::set<double>{ std::round(top->merged_height * 1000.) / 1000. });
        if (top->top_idx - top->bottom_idx + 1 == expect.per_slab) {
            ++full_slabs;
            CHECK_THAT(top->merged_height, WithinAbs(expect.slab, 1e-6));
        }
    }
    // N fine layers per coarse slab, apart from at most a short tail at the top.
    CHECK(full_slabs + 1 >= (layers - 1) / expect.per_slab);
    CHECK(slab_tops <= (layers - 1) / expect.per_slab + 1);

    // ToolOrdering follows the slabs: filament 2 is on layer 0 and on the slab tops only, filament 1 everywhere.
    ToolOrdering ordering(print, (unsigned int) -1, false);
    size_t       layers_with_2 = 0;
    for (const LayerTools &lt : ordering.layer_tools()) {
        CAPTURE(lt.print_z);
        const bool has_1 = std::find(lt.extruders.begin(), lt.extruders.end(), 0u) != lt.extruders.end();
        const bool has_2 = std::find(lt.extruders.begin(), lt.extruders.end(), 1u) != lt.extruders.end();
        CHECK(has_1);
        CHECK(has_2 == (coarse_print_z.count(std::round(lt.print_z * 1000.) / 1000.) > 0));
        layers_with_2 += has_2;
    }
    CHECK(layers_with_2 == slab_tops + 1);

    // G-code: one ;LAYER_CHANGE per object layer with strictly rising ;Z:, T0 extrudes on every layer at the base
    // height, T1 extrudes on layer 0 and on the slab tops only, at the slab height.
    const std::vector<GcodeLayer> glayers = parse_layers(gcode);
    REQUIRE(glayers.size() == layers);
    size_t t1_layers = 0;
    for (size_t idx = 0; idx < glayers.size(); ++idx) {
        CAPTURE(idx);
        if (idx > 0)
            CHECK(glayers[idx].z > glayers[idx - 1].z);
        CHECK(glayers[idx].tools.count(0) == 1);
        const bool t1 = glayers[idx].tools.count(1) == 1;
        CHECK(t1 == (coarse_print_z.count(std::round(glayers[idx].z * 1000.) / 1000.) > 0));
        if (! t1)
            continue;
        ++t1_layers;
        // Every T1 extrusion of the layer carries the slab's height (the base height on layer 0).
        const double slab = coarse_slab_height.at(std::round(glayers[idx].z * 1000.) / 1000.);
        CHECK(glayers[idx].heights.at(1) == std::set<double>{ std::round(slab * 1000.) / 1000. });
    }
    CHECK(t1_layers == slab_tops + 1);
    // The processor ran over the whole file (preview and time estimate).
    CHECK(! result.moves.empty());
}

} // namespace

TEST_CASE("Adaptive layer merge live: a 0.4 + 0.8 plate stacks two 0.4 layers per 0.8 slab", "[AdaptiveLayerMergeLive]")
{
    check_mixed_plate(h3c_like_04_08(), 10., { 0.2, 0.4, 2 });
}

TEST_CASE("Adaptive layer merge live: an H2D-like 0.2 L / 0.4 R plate stacks two 0.2 layers per 0.4 slab", "[AdaptiveLayerMergeLive]")
{
    check_mixed_plate(h2d_like_02_04(), 6., { 0.1, 0.2, 2 });
}

// The preview's Layer height legend on a mixed plate (owner report, 2026-11-06: 0.20 to 5.80 mm, every layer dark
// blue). The G-code's ;HEIGHT: tags and the processor's heights are only ever the base or the slab height, the
// prime tower's included. The 5.8 came from the start G-code: the H3C load line extrudes 0.8 mm above a Z5 move,
// before any height tag, so the processor guesses its height from Z. The legend now skips custom G-code extrusions
// for its height and width ranges, as BambuStudio does (tests/slic3rutils/libvgcode_input_tests.cpp).
TEST_CASE("Adaptive layer merge live: a 0.4 + 0.8 plate's height tags are the base and slab heights", "[AdaptiveLayerMergeLive]")
{
    const bool tower = GENERATE(false, true);
    CAPTURE(tower);
    DynamicPrintConfig config = h3c_like_04_08();
    config.option<ConfigOptionFloats>("wipe_tower_x")->values = { 15. };
    config.option<ConfigOptionFloats>("wipe_tower_y")->values = { 15. };
    config.set_deserialize_strict({
        { "enable_prime_tower", tower ? "1" : "0" },
        { "prime_tower_width", "35" },
        // the H3C start G-code's nozzle load line, with no height tag ahead of it
        { "machine_start_gcode", "G90\nM83\nG1 Z5 F1200\nG91\nG1 Z0.8 F1200\nG90\nG1 X10 Y0 F6000\nG1 X50 E10 F600\n" },
    });
    Model model;
    add_cubes(model, 10.);
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.uses_multiple_nozzle_diameters());
    GCodeProcessorResult result;
    const std::string    gcode = Test::gcode(print, result);
    REQUIRE(! gcode.empty());

    const double                base = 0.2, slab = 0.4;
    const std::set<double>      allowed{ base, slab };
    auto                        rounded = [](double v) { return std::round(v * 1000.) / 1000.; };

    // Every ;HEIGHT: tag from the first layer on: layer changes, paths and the prime tower.
    std::set<double>        tags;
    std::istringstream      in(gcode);
    std::string             line;
    bool                    in_layers = false;
    static const std::regex height_re(R"(^;HEIGHT:([0-9.]+))");
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";LAYER_CHANGE", 0) == 0)
            in_layers = true;
        std::smatch m;
        if (in_layers && std::regex_search(line, m, height_re))
            tags.insert(rounded(std::stod(m[1].str())));
    }
    REQUIRE(! tags.empty());
    CHECK(*tags.rbegin() == slab);
    for (double h : tags) {
        CAPTURE(h);
        CHECK(allowed.count(h) == 1);
    }

    // The processor's heights, which the legend reads: base to slab for everything but custom G-code.
    float            min_h = std::numeric_limits<float>::max(), max_h = 0.f, custom_max = 0.f;
    std::set<double> move_heights;
    bool             tower_seen = false;
    for (const GCodeProcessorResult::MoveVertex &move : result.moves) {
        if (move.type != EMoveType::Extrude)
            continue;
        if (move.extrusion_role == erCustom) {
            custom_max = std::max(custom_max, move.height);
            continue;
        }
        tower_seen |= move.extrusion_role == erWipeTower;
        min_h = std::min(min_h, move.height);
        max_h = std::max(max_h, move.height);
        if (! move.internal_only)
            move_heights.insert(rounded(move.height));
    }
    CHECK(tower_seen == tower);
    CHECK_THAT(min_h, WithinAbs(base, 1e-4));
    CHECK_THAT(max_h, WithinAbs(slab, 1e-4));
    for (double h : move_heights) {
        CAPTURE(h);
        CHECK(allowed.count(h) == 1);
    }
    // The load line is what reached 5.8: kept out of the legend by the viewer, not by the processor.
    CHECK(custom_max > 1.f);
}

TEST_CASE("Adaptive layer merge live: a configured max layer height keeps the coarse nozzle on the base grid", "[AdaptiveLayerMergeLive]")
{
    // The 0.8's own ceiling of 0.3 rules out a 0.4 slab: the per-nozzle clamp leaves it printing every layer.
    DynamicPrintConfig config = two_nozzles("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.3");
    Model model;
    add_cubes(model, 6.);
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    REQUIRE(print.uses_multiple_nozzle_diameters());
    const std::string gcode = Test::gcode(print);
    const PrintObject *coarse = object_named(print, "cube-2");
    REQUIRE(coarse != nullptr);
    for (const Layer *layer : coarse->layers()) {
        for (const LayerRegion *layerm : layer->regions())
            CHECK(layerm->adaptive_merge_patches.empty());
        CHECK(layer->has_extrusions());
    }
}

// #907 report: the 0.8's own min_layer_height of 0.24 at 0.20 layers leaves the clamp [0.24, 0.56] one band height
// only (0.4). The 49 layers above the bed contact of a 10 mm cube cannot be cut into 0.4 slabs, the ladder came
// back empty and the 0.8 printed layer 0 and nothing else. The ceiling is now relaxed to the nozzle diameter:
// 0.4 slabs with one 0.6 slab, every slab at or above the configured minimum.
TEST_CASE("Adaptive layer merge live: a configured min layer height that leaves one slab height still prints the whole object", "[AdaptiveLayerMergeLive]")
{
    const DynamicPrintConfig config = two_nozzles("0.4,0.8", "0.2", "0.08,0.24", "0.28,0.56");
    check_mixed_plate(config, 10., { 0.2, 0.4, 2 });

    Model model;
    add_cubes(model, 10.);
    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(! gcode.empty());
    const PrintObject *coarse = object_named(print, "cube-2");
    REQUIRE(coarse != nullptr);
    const size_t layers = coarse->layer_count();
    REQUIRE(layers == 50);
    // The cube's top layer is a slab top: the object reaches its full height.
    const AdaptiveMergePatch *last = group_top_patch(*coarse->layers().back());
    REQUIRE(last != nullptr);
    CHECK(coarse->layers().back()->has_extrusions());
    double printed = coarse->layers().front()->height;
    for (size_t idx = 1; idx < layers; ++idx) {
        CAPTURE(idx);
        if (const AdaptiveMergePatch *top = group_top_patch(*coarse->layers()[idx]); top != nullptr) {
            CHECK(top->merged_height >= 0.24 - EPSILON);
            CHECK(top->merged_height <= 0.8 + EPSILON);
            printed += top->merged_height;
        }
    }
    CHECK_THAT(printed, WithinAbs(10., 1e-6));
}

TEST_CASE("Adaptive layer merge: the ladder window widens only a clamp that holds a single band height", "[AdaptiveLayerMergeLive]")
{
    auto check = [](AdaptiveLayerHeightRange clamp, double nozzle, double reference, double base, double min_h, double max_h) {
        const AdaptiveLayerHeightRange out = adaptive_ladder_height_range(clamp, nozzle, reference, base);
        CHECK_THAT(out.min_height, WithinAbs(min_h, 1e-9));
        CHECK_THAT(out.max_height, WithinAbs(max_h, 1e-9));
    };
    // The plain 0.8 window holds 0.2 and 0.4 at 0.2 layers: unchanged.
    check({ 0.16, 0.56 }, 0.8, 0.4, 0.2, 0.16, 0.56);
    // min 0.24: 0.4 only. The ceiling goes to 0.8, the minimum stays.
    check({ 0.24, 0.56 }, 0.8, 0.4, 0.2, 0.24, 0.8);
    // max 0.3: the 0.8 does not merge at all (band size 1), so nothing is widened.
    check({ 0.16, 0.3 }, 0.8, 0.4, 0.2, 0.16, 0.3);
    // min 0.5 at 0.27 layers: 0.54 only, and 0.81 is past the nozzle too. Bambu's diameter window.
    check({ 0.5, 0.56 }, 0.8, 0.4, 0.27, 0.16, 0.56);

    // The cause: under the raw clamp the 49 layers above a 10 mm cube's bed contact have no cover at all, so the
    // ladder is empty. The widened window cuts them into 23 slabs of 0.4 and one of 0.6.
    const std::vector<double> stack(50, 0.2);
    CHECK(build_adaptive_layer_ladder_for_test(stack, { 0.24, 0.56 }).empty());
    const std::vector<AdaptiveLayerBand> bands = build_adaptive_layer_ladder_for_test(stack, { 0.24, 0.8 });
    REQUIRE(bands.size() == 24);
    CHECK(bands.front().bottom_idx == 1);
    CHECK(bands.back().top_idx == 49);
    for (size_t i = 1; i < bands.size(); ++i)
        CHECK(bands[i].bottom_idx == bands[i - 1].top_idx + 1);
}

TEST_CASE("Adaptive layer merge live: a single-diameter plate plans nothing and prints every layer as sliced", "[AdaptiveLayerMergeLive]")
{
    Model model;
    add_cubes(model, 6.);
    Print print;
    print.apply(model, same_diameter_04());
    print.validate();
    print.set_status_silent();
    REQUIRE_FALSE(print.uses_multiple_nozzle_diameters());
    const std::string gcode = Test::gcode(print);
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers()) {
            for (const LayerRegion *layerm : layer->regions())
                CHECK(layerm->adaptive_merge_patches.empty());
            CHECK(layer->has_extrusions());
            CHECK(layer_heights_of(*layer) == std::set<double>{ std::round(layer->height * 1000.) / 1000. });
        }
    // Every tool extrudes on every layer, each at the layer's own height.
    const std::vector<GcodeLayer> glayers = parse_layers(gcode);
    REQUIRE(! glayers.empty());
    for (const GcodeLayer &l : glayers) {
        CHECK(l.tools == std::set<int>{ 0, 1 });
        for (const auto &kv : l.heights)
            CHECK(kv.second.size() == 1);
    }
}

struct OverhangSeen
{
    bool overhang_wall_on_slab { false };
    bool own_bottom_on_slab { false };
    bool lifted_bridge_on_slab { false };
};

// Slices `coarse_object` (put on filament 2, the 0.8) next to an 8 mm cube on filament 1 (the 0.4) and checks every
// slab of the coarse object: overhang walls at the slab height, the slab top's own bottom faces unmerged, bridges
// lifted from swallowed steps at the slab height, swallowed steps without extrusions or fill.
OverhangSeen check_overhang_slabs(Model &model, ModelObject *coarse_object)
{
    coarse_object->name = "overhang";
    coarse_object->volumes.front()->config.set("extruder", 2);
    ModelObject *fine_object = model.add_object();
    fine_object->name        = "cube-1";
    fine_object->add_volume(TriangleMesh(its_make_cube(8., 8., 8.)));
    fine_object->add_instance()->set_offset(Vec3d(60., 100., 0.));
    fine_object->ensure_on_bed();
    fine_object->volumes.front()->config.set("extruder", 1);

    OverhangSeen seen;
    Print print;
    print.apply(model, h3c_like_04_08());
    print.validate();
    print.set_status_silent();
    REQUIRE(print.uses_multiple_nozzle_diameters());
    const std::string gcode = Test::gcode(print);
    REQUIRE(! gcode.empty());
    const PrintObject *coarse = object_named(print, "overhang");
    REQUIRE(coarse != nullptr);
    REQUIRE(coarse->layer_count() > 10);

    for (size_t idx = 1; idx < coarse->layer_count(); ++idx) {
        const Layer *layer = coarse->layers()[idx];
        CAPTURE(idx);
        const AdaptiveMergePatch *top = group_top_patch(*layer);
        if (top == nullptr) {
            bool has_patches = false;
            for (const LayerRegion *layerm : layer->regions())
                has_patches |= ! layerm->adaptive_merge_patches.empty();
            if (! has_patches)
                continue; // a step the planner left on the fine grid prints as sliced
            // Swallowed: no extrusions and no fill left to print.
            CHECK_FALSE(layer->has_extrusions());
            for (const LayerRegion *layerm : layer->regions())
                CHECK(layerm->fill_surfaces.empty());
            continue;
        }
        const bool slab = top->merged_height > layer->height + EPSILON;
        // Every wall (overhang walls included), gap fill and non-bridge infill is laid at the slab height.
        CHECK(layer_heights_of(*layer) == std::set<double>{ std::round(top->merged_height * 1000.) / 1000. });
        for (const LayerRegion *layerm : layer->regions()) {
            for (const ExtrusionEntity *e : layerm->perimeters.entities) {
                std::vector<const ExtrusionPath *> paths;
                std::vector<const ExtrusionEntity *> stack { e };
                while (! stack.empty()) {
                    const ExtrusionEntity *cur = stack.back();
                    stack.pop_back();
                    if (const auto *c = dynamic_cast<const ExtrusionEntityCollection *>(cur))
                        for (const ExtrusionEntity *child : c->entities)
                            stack.push_back(child);
                    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(cur))
                        for (const ExtrusionPath &p : loop->paths)
                            paths.push_back(&p);
                    else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(cur))
                        for (const ExtrusionPath &p : multi->paths)
                            paths.push_back(&p);
                    else if (const auto *path = dynamic_cast<const ExtrusionPath *>(cur))
                        paths.push_back(path);
                }
                for (const ExtrusionPath *path : paths)
                    if (path->role() == erOverhangPerimeter && slab) {
                        seen.overhang_wall_on_slab = true;
                        CHECK_THAT(path->height, WithinAbs(top->merged_height, 1e-4));
                    }
            }
            // The slab top's own bottom faces stay unmerged (no merged thickness); everything else in the slab
            // carries the slab's height.
            for (const Surface &surface : layerm->fill_surfaces.surfaces) {
                if (surface.surface_type == stInternalVoid)
                    continue;
                if (surface.is_bottom() && surface.thickness < 0.) {
                    // The slab top's own bottom face: unmerged.
                    seen.own_bottom_on_slab |= slab;
                } else if (surface.is_bottom()) {
                    // A bridge lifted from a swallowed step keeps its type at the slab height; a non-bridge bottom of a
                    // swallowed step would have turned internal solid.
                    CHECK(surface.is_bridge());
                    CHECK_THAT(surface.thickness, WithinAbs(top->merged_height, 1e-9));
                    seen.lifted_bridge_on_slab = true;
                } else {
                    CHECK_THAT(surface.thickness, WithinAbs(top->merged_height, 1e-9));
                }
            }
        }
    }
    return seen;
}

TEST_CASE("Adaptive layer merge live: overhang.obj on the 0.8 - overhang walls at the slab height", "[AdaptiveLayerMergeLive]")
{
    // tests/data/overhang.obj on the 0.8, a cube on the 0.4.
    Model source = Model::read_from_file(std::string(TEST_DATA_DIR) + "/overhang.obj");
    REQUIRE(! source.objects.empty());
    Model model;
    ModelObject *coarse_object = model.add_object(*source.objects.front());
    coarse_object->clear_instances();
    coarse_object->add_instance();
    coarse_object->center_around_origin();
    coarse_object->instances.front()->set_offset(Vec3d(130., 100., 0.));
    coarse_object->ensure_on_bed();
    const OverhangSeen seen = check_overhang_slabs(model, coarse_object);
    CHECK(seen.overhang_wall_on_slab);
}

TEST_CASE("Adaptive layer merge live: a face starting on a slab top keeps its bottom on the fine grid", "[AdaptiveLayerMergeLive]")
{
    // A 4 mm pillar up to z 2, a 10 mm plate from 2 to 3, a 16 mm plate from 3 to 4, on the 0.8 at a 0.2 base. The
    // ladder starts on layer 1 in slabs of 2, so slab tops are the even layers. The z 2 face first shows on layer 10
    // (slice z 2.1), a slab top: its bottom stays unmerged. The z 3 face first shows on layer 15, the swallowed step of
    // [15, 16]: its bridge is lifted onto layer 16 at the slab height.
    TriangleMesh stepped(its_make_cube(4., 4., 2.));
    stepped.translate(6.f, 6.f, 0.f);
    TriangleMesh mid(its_make_cube(10., 10., 1.));
    mid.translate(3.f, 3.f, 2.f);
    stepped.merge(mid);
    TriangleMesh wide(its_make_cube(16., 16., 1.));
    wide.translate(0.f, 0.f, 3.f);
    stepped.merge(wide);
    Model model;
    ModelObject *coarse_object = model.add_object();
    coarse_object->add_volume(std::move(stepped));
    coarse_object->add_instance()->set_offset(Vec3d(130., 100., 0.));
    coarse_object->ensure_on_bed();
    const OverhangSeen seen = check_overhang_slabs(model, coarse_object);
    CHECK(seen.overhang_wall_on_slab);
    CHECK(seen.own_bottom_on_slab);
    CHECK(seen.lifted_bridge_on_slab);
}
