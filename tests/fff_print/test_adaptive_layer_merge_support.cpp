// Mixed nozzle diameters, step 4 of the BambuStudio v02.09.01.70 AdaptiveLayerMerge port: supports follow the merged
// slabs. Support printed by a filament on a coarser nozzle than the band reference stacks in the slab heights the
// walls of that nozzle use; support printed by the reference (fine) nozzle stays on the base grid. A plate printed
// through one nozzle diameter is untouched.

#include <catch2/catch.hpp>

#include "libslic3r/libslic3r.h"
#include "libslic3r/AdaptiveLayerMerge.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/SupportMergePlan.hpp"
#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
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

double r3(double v) { return std::round(v * 1000.) / 1000.; }

// Two tools of the given diameters, one filament each, supports on. `layer` is both the first and the base layer height.
// support / interface: the 1-based support_filament / support_interface_filament ("0" = don't care).
DynamicPrintConfig support_plate(const char *diameters, const char *layer, const char *min_heights, const char *max_heights,
                                 const char *support, const char *interface, const char *type, const char *style)
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
        { "enable_support", "1" },
        { "support_type", type },
        { "support_style", style },
        { "support_filament", support },
        { "support_interface_filament", interface },
        { "independent_support_layer_height", "0" },
        { "support_top_z_distance", "0.2" },
        { "support_bottom_z_distance", "0.2" },
        { "support_interface_top_layers", "2" },
        { "support_interface_bottom_layers", "0" },
        { "support_on_build_plate_only", "1" },
    });
    return config;
}

// A mushroom on filament 2: a 4 mm pillar up to `stem` mm and a 16 mm plate 1 mm thick on top of it, so the plate's
// overhang ring needs support from the bed. Next to it an 8 mm block on filament 1.
void add_mushroom_plate(Model &model, float stem)
{
    TriangleMesh mushroom(its_make_cube(4., 4., stem));
    mushroom.translate(6.f, 6.f, 0.f);
    TriangleMesh cap(its_make_cube(16., 16., 1.));
    cap.translate(0.f, 0.f, stem);
    mushroom.merge(cap);
    ModelObject *coarse = model.add_object();
    coarse->name        = "mushroom";
    coarse->add_volume(std::move(mushroom));
    coarse->add_instance()->set_offset(Vec3d(120., 100., 0.));
    coarse->ensure_on_bed();
    coarse->volumes.front()->config.set("extruder", 2);

    ModelObject *fine = model.add_object();
    fine->name        = "cube-1";
    // As tall as the mushroom, so both objects share every G-code layer.
    fine->add_volume(TriangleMesh(its_make_cube(8., 8., double(stem) + 1.)));
    fine->add_instance()->set_offset(Vec3d(60., 100., 0.));
    fine->ensure_on_bed();
    fine->volumes.front()->config.set("extruder", 1);
}

const PrintObject *object_named(const Print &print, const std::string &name)
{
    for (const PrintObject *object : print.objects())
        if (object->model_object()->name == name)
            return object;
    return nullptr;
}

struct PathInfo
{
    ExtrusionRole role;
    double        height;
};

void collect_paths(const ExtrusionEntity &entity, std::vector<PathInfo> &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            collect_paths(*child, out);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        out.push_back({ path->role(), path->height });
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multi->paths)
            out.push_back({ p.role(), p.height });
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            out.push_back({ p.role(), p.height });
    }
}

std::vector<PathInfo> support_paths(const SupportLayer &layer)
{
    std::vector<PathInfo> out;
    for (const ExtrusionEntity *e : layer.support_fills.entities)
        collect_paths(*e, out);
    return out;
}

// The coarse object's slab tops: rounded print_z -> slab height. Layer 0 is in it at its own height.
std::map<double, double> slab_tops_of(const PrintObject &object)
{
    std::map<double, double> tops;
    const Layer *first = object.layers().front();
    tops[r3(first->print_z)] = first->height;
    for (size_t idx = 1; idx < object.layer_count(); ++ idx) {
        const Layer *layer = object.layers()[idx];
        for (const LayerRegion *layerm : layer->regions())
            for (const AdaptiveMergePatch &patch : layerm->adaptive_merge_patches)
                if (patch.group_top && ! patch.area().empty())
                    tops[r3(layer->print_z)] = std::max(tops[r3(layer->print_z)], patch.merged_height);
    }
    return tops;
}

struct GcodeLayer
{
    double                          z { 0. };
    std::set<int>                   tools;
    std::map<int, std::set<double>> heights; // tool -> ;HEIGHT: values of its non-bridge extruding moves
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
            bridge = m[1].str() == "Bridge" || m[1].str() == "Internal Bridge";
            continue;
        }
        if (std::regex_search(line, m, tool_re)) {
            tool = std::stoi(m[1].str());
            continue;
        }
        if (! out.empty() && line.compare(0, 2, "G1") == 0 && std::regex_search(line, e_re) && line.find(" X") != std::string::npos) {
            out.back().tools.insert(tool);
            if (! bridge)
                out.back().heights[tool].insert(r3(height));
        }
    }
    return out;
}

struct SlicedPlate
{
    Print             print;
    std::string       gcode;
    const PrintObject *coarse { nullptr };
};

void slice(SlicedPlate &plate, const DynamicPrintConfig &config, float stem)
{
    Model model;
    add_mushroom_plate(model, stem);
    plate.print.apply(model, config);
    plate.print.validate();
    plate.print.set_status_silent();
    plate.gcode = Test::gcode(plate.print);
    REQUIRE(! plate.gcode.empty());
    plate.coarse = object_named(plate.print, "mushroom");
    REQUIRE(plate.coarse != nullptr);
    REQUIRE(plate.coarse->support_layer_count() > 4);
}

// Support printed by the coarse filament: the whole support stack (base and interface) prints on layer 0 and on the
// coarse slab tops only, every path of it at the slab height, and in the G-code T1 extrudes on exactly those layers.
void check_support_on_slabs(const DynamicPrintConfig &config, float stem, double base, double slab)
{
    SlicedPlate plate;
    slice(plate, config, float(stem));
    REQUIRE(plate.print.uses_multiple_nozzle_diameters());
    const PrintObject &coarse = *plate.coarse;

    // The plan the supports were merged with: filament 2's fixed ladder, starting on layer 1.
    const SupportMergePlan plan = make_support_merge_plan(coarse);
    REQUIRE(plan.enabled);
    REQUIRE(plan.fixed_bands_per_filament.count(2) == 1);
    REQUIRE(! plan.slabs.empty());
    CHECK(plan.slab_for_layer(0) == nullptr);
    for (const SupportMergePlan::Slab &s : plan.slabs) {
        CHECK(s.filament_id == 2);
        CHECK(s.bottom_idx >= 1);
    }

    const std::map<double, double> tops = slab_tops_of(coarse);
    const double                   z0   = r3(coarse.layers().front()->print_z);
    size_t merged_layers = 0;
    for (const SupportLayer *layer : coarse.support_layers()) {
        if (! layer->has_extrusions())
            continue;
        const double z = r3(layer->print_z);
        CAPTURE(z);
        // Support prints on layer 0 and on the slab tops only.
        REQUIRE(tops.count(z) == 1);
        const double height = z == z0 ? base : tops.at(z);
        for (const PathInfo &path : support_paths(*layer))
            CHECK_THAT(path.height, WithinAbs(height, 1e-6));
        if (z != z0) {
            ++ merged_layers;
            CHECK_THAT(layer->effective_print_height(), WithinAbs(tops.at(z), 1e-6));
        }
    }
    CHECK(merged_layers >= 3);

    // G-code: T1 (the coarse filament: the mushroom and its support) extrudes on layer 0 and the slab tops only, at
    // the slab height; T0 (the cube) on every layer at the base height.
    const std::vector<GcodeLayer> glayers = parse_layers(plate.gcode);
    REQUIRE(glayers.size() == coarse.layer_count());
    for (size_t idx = 0; idx < glayers.size(); ++ idx) {
        CAPTURE(idx);
        const double z = r3(glayers[idx].z);
        if (idx > 0)
            CHECK(glayers[idx].z > glayers[idx - 1].z);
        CHECK(glayers[idx].tools.count(0) == 1);
        const bool t1 = glayers[idx].tools.count(1) == 1;
        CHECK(t1 == (tops.count(z) == 1));
        // At the slab height, apart from the cap's own bottom face, which step 3 keeps on the fine grid.
        if (t1 && glayers[idx].heights.count(1))
            for (double h : glayers[idx].heights.at(1))
                CHECK((h == r3(tops.at(z)) || h == r3(base)));
        if (glayers[idx].heights.count(0))
            CHECK(glayers[idx].heights.at(0) == std::set<double>{ r3(base) });
    }
    (void) slab;
}

} // namespace

TEST_CASE("Adaptive layer merge support: H3C-like 0.4 + 0.8, support from the 0.8 stacks in its slabs", "[AdaptiveLayerMergeSupport]")
{
    // The plate's first layer starts at z 4, the contact sits a 0.2 gap below it on the 0.8's even ladder top.
    check_support_on_slabs(support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "2", "2", "normal(auto)", "default"),
                           4.f, 0.2, 0.4);
}

TEST_CASE("Adaptive layer merge support: H3C-like, don't care support on the 0.8 object is pinned to the 0.8", "[AdaptiveLayerMergeSupport]")
{
    // support_filament 0: the plan resolves "whatever the object prints with" per layer, the 0.8 here, and pins the
    // slabs to it so the exporter cannot hand them to the 0.4.
    check_support_on_slabs(support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "0", "0", "normal(auto)", "default"),
                           4.f, 0.2, 0.4);
    SlicedPlate plate;
    slice(plate, support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "0", "0", "normal(auto)", "default"), 4.f);
    size_t pinned = 0;
    for (const SupportLayer *layer : plate.coarse->support_layers())
        if (layer->planned_filament_id != 0) {
            CHECK(layer->planned_filament_id == 2);
            CHECK_THAT(layer->planned_nozzle_diameter, WithinAbs(0.8, 1e-9));
            ++ pinned;
        }
    CHECK(pinned >= 3);
}

TEST_CASE("Adaptive layer merge support: H3C-like, support from the 0.4 stays on the base grid", "[AdaptiveLayerMergeSupport]")
{
    SlicedPlate plate;
    slice(plate, support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "1", "1", "normal(auto)", "default"), 4.f);
    REQUIRE(plate.print.uses_multiple_nozzle_diameters());
    // The mushroom itself still stacks in 0.4 slabs.
    const std::map<double, double> tops = slab_tops_of(*plate.coarse);
    REQUIRE(tops.size() > 3);
    size_t fine_layers = 0;
    for (const SupportLayer *layer : plate.coarse->support_layers()) {
        CHECK(layer->planned_filament_id == 0);
        CHECK(layer->base_slab_height == 0.);
        CHECK(layer->interface_slab_height == 0.);
        if (! layer->has_extrusions())
            continue;
        CAPTURE(layer->print_z);
        for (const PathInfo &path : support_paths(*layer))
            CHECK_THAT(path.height, WithinAbs(0.2, 1e-6));
        fine_layers += tops.count(r3(layer->print_z)) == 0;
    }
    // Support on the layers between the 0.8's slab tops too: one support layer per base layer below the contact.
    CHECK(fine_layers >= 8);
    // The supports print with T0 at 0.2 on every layer, T1 prints the mushroom on its slab tops only.
    for (const GcodeLayer &l : parse_layers(plate.gcode)) {
        CAPTURE(l.z);
        CHECK(l.tools.count(1) == tops.count(r3(l.z)));
        if (l.heights.count(0))
            CHECK(l.heights.at(0) == std::set<double>{ 0.2 });
    }
}

TEST_CASE("Adaptive layer merge support: H2D-like 0.2 L / 0.4 R, support from the 0.4 stacks in 0.2 slabs", "[AdaptiveLayerMergeSupport]")
{
    // At a 0.1 base the 0.4's ladder tops are the even layers; a stem of 4.1 puts the contact (0.2 gap) on one.
    check_support_on_slabs(support_plate("0.2,0.4", "0.1", "0.04,0.08", "0.14,0.28", "2", "2", "normal(auto)", "default"),
                           4.1f, 0.1, 0.2);
}

// Tree supports: every base folded into a slab sits on a slab top of the coarse object, carries the slab height and
// is pinned to the coarse filament; nothing of the base prints thicker anywhere else.
void check_tree_base_slabs(const char *style)
{
    SlicedPlate plate;
    slice(plate, support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "2", "2", "tree(auto)", style), 4.f);
    REQUIRE(plate.print.uses_multiple_nozzle_diameters());
    const std::map<double, double> tops = slab_tops_of(*plate.coarse);
    size_t slab_layers = 0;
    for (const SupportLayer *layer : plate.coarse->support_layers()) {
        const double z = r3(layer->print_z);
        CAPTURE(z);
        const bool on_top = tops.count(z) == 1;
        if (layer->base_slab_height > EPSILON) {
            ++ slab_layers;
            REQUIRE(on_top);
            CHECK_THAT(layer->base_slab_height, WithinAbs(tops.at(z), 1e-6));
            CHECK(layer->planned_filament_id == 2);
            CHECK_THAT(layer->effective_print_height(), WithinAbs(tops.at(z), 1e-6));
        }
        for (const PathInfo &path : support_paths(*layer))
            if (path.role == erSupportMaterial && path.height > layer->height + EPSILON) {
                // A base path thicker than its layer is a folded slab, so it is on a slab top at the slab height.
                CHECK(on_top);
                CHECK(layer->base_slab_height > EPSILON);
                CHECK_THAT(path.height, WithinAbs(layer->base_slab_height, 1e-6));
            }
    }
    CHECK(slab_layers >= 3);
}

TEST_CASE("Adaptive layer merge support: classic tree base folds onto the 0.8 slabs", "[AdaptiveLayerMergeSupport]")
{
    check_tree_base_slabs("tree_hybrid");
}

TEST_CASE("Adaptive layer merge support: organic tree base merges onto the 0.8 slabs", "[AdaptiveLayerMergeSupport]")
{
    check_tree_base_slabs("organic");
}

// Owner crash 2026-11-07: H3C 0.4 + 0.8 rack, a mushroom with organic tree supports, crashed every time in "Generating
// support". A base layer merged into a 0.4 slab of the 0.8 overlaps the top contacts inside its Z span, so
// modulate_extrusion_by_overlapping_layers() saw the double-walled branches tree_supports_generate_paths() wraps in an
// ExtrusionEntityCollection and read them as a null ExtrusionPath. A stepped cap (a new overhang every 0.6 mm, off the
// 0.4 ladder) puts top contacts inside the slabs; tree_support_wall_count 2 double-walls every branch; the 0.8 nozzle's
// own process prints 0.40 layers on the 0.20 plate (nozzle_layer_height, #910) as in the owner's project.
TEST_CASE("Adaptive layer merge support: organic tree slabs with double-walled branches under a stepped cap", "[AdaptiveLayerMergeSupport]")
{
    DynamicPrintConfig config = support_plate("0.4,0.8", "0.2", "0.08,0.16", "0.28,0.56", "0", "0", "tree(auto)", "organic");
    config.set_deserialize_strict({
        { "tree_support_wall_count", "2" },
        { "nozzle_layer_height", "0,0.4" },
        { "support_on_build_plate_only", "0" },
    });

    const float stem = 4.f;
    const float top  = stem + 6 * 0.6f + 1.f;
    TriangleMesh mushroom(its_make_cube(4., 4., stem));
    mushroom.translate(-2.f, -2.f, 0.f);
    for (int k = 0; k < 6; ++ k) {
        const float half = 2.f + 1.2f * float(k + 1);
        TriangleMesh plate(its_make_cube(2. * half, 2. * half, top - (stem + 0.6f * float(k))));
        plate.translate(-half, -half, stem + 0.6f * float(k));
        mushroom.merge(plate);
    }
    Model model;
    ModelObject *coarse_object = model.add_object();
    coarse_object->name        = "mushroom";
    coarse_object->add_volume(std::move(mushroom));
    coarse_object->add_instance()->set_offset(Vec3d(120., 100., 0.));
    coarse_object->ensure_on_bed();
    coarse_object->volumes.front()->config.set("extruder", 2);
    ModelObject *fine_object = model.add_object();
    fine_object->name        = "cube-1";
    fine_object->add_volume(TriangleMesh(its_make_cube(8., 8., double(top))));
    fine_object->add_instance()->set_offset(Vec3d(60., 100., 0.));
    fine_object->ensure_on_bed();
    fine_object->volumes.front()->config.set("extruder", 1);

    Print print;
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode = Test::gcode(print);
    REQUIRE(! gcode.empty());
    REQUIRE(print.uses_multiple_nozzle_diameters());
    const PrintObject *coarse = object_named(print, "mushroom");
    REQUIRE(coarse != nullptr);

    // The case that crashed: a merged base slab carrying double-walled branches.
    size_t slabs_with_branches = 0;
    for (const SupportLayer *layer : coarse->support_layers()) {
        if (layer->base_slab_height <= EPSILON)
            continue;
        CAPTURE(layer->print_z);
        CHECK(layer->planned_filament_id == 2);
        bool branches = false;
        for (const ExtrusionEntity *e : layer->support_fills.entities)
            branches |= dynamic_cast<const ExtrusionEntityCollection *>(e) != nullptr;
        slabs_with_branches += branches;
        // Every base path of the slab, the branches' walls included, keeps the full slab height or a modulated
        // smaller one; none prints thicker than the slab.
        for (const PathInfo &path : support_paths(*layer))
            if (path.role == erSupportMaterial)
                CHECK(path.height <= layer->base_slab_height + EPSILON);
    }
    CHECK(slabs_with_branches >= 1);
}

TEST_CASE("Adaptive layer merge support: a single-diameter plate plans nothing for its supports", "[AdaptiveLayerMergeSupport]")
{
    for (const char *type : { "normal(auto)", "tree(auto)" }) {
        CAPTURE(type);
        SlicedPlate plate;
        slice(plate, support_plate("0.4,0.4", "0.2", "0.08,0.08", "0.28,0.28", "0", "0", type, "default"), 4.f);
        REQUIRE_FALSE(plate.print.uses_multiple_nozzle_diameters());
        CHECK_FALSE(make_support_merge_plan(*plate.coarse).enabled);
        for (const SupportLayer *layer : plate.coarse->support_layers()) {
            CHECK(layer->planned_filament_id == 0);
            CHECK(layer->planned_interface_filament_id == 0);
            CHECK(layer->base_slab_height == 0.);
            CHECK(layer->interface_slab_height == 0.);
            CHECK(layer->effective_print_height() == layer->height);
        }
    }
}

// "Don't care" support and the nozzle that can print its height (BambuStudio v02.09.01.70 GCode.cpp:5310-5330,
// nozzle_can_print_height). A 0.4 object with two overhangs at different heights: a ledge on +x at z 4 and a cap on
// -x at z 6, so on the ledge's contact/interface levels the same support layer carries the ledge's interface and the
// cap's base. Next to it a block on filament 2 on a 0.8 nozzle. At 0.12 layers the 0.8 prints in 0.24 slabs and
// cannot extrude a 0.12 support layer (its window is 0.16..0.56), which the 0.4 can (0.08..0.28): the H3C rack case.
namespace {

void add_ledge_plate(Model &model)
{
    TriangleMesh ledge(its_make_cube(4., 4., 7.));
    ledge.translate(6.f, 6.f, 0.f);
    TriangleMesh low(its_make_cube(6., 12., 1.));
    low.translate(10.f, 2.f, 4.f);
    ledge.merge(low);
    TriangleMesh high(its_make_cube(6., 12., 1.));
    high.translate(0.f, 2.f, 6.f);
    ledge.merge(high);
    ModelObject *fine = model.add_object();
    fine->name        = "ledge";
    fine->add_volume(std::move(ledge));
    fine->add_instance()->set_offset(Vec3d(60., 100., 0.));
    fine->ensure_on_bed();
    fine->volumes.front()->config.set("extruder", 1);

    ModelObject *coarse = model.add_object();
    coarse->name        = "block";
    // Taller than the ledge object, so filament 2 is on the layers of both overhangs.
    coarse->add_volume(TriangleMesh(its_make_cube(8., 8., 8.)));
    coarse->add_instance()->set_offset(Vec3d(120., 100., 0.));
    coarse->ensure_on_bed();
    coarse->volumes.front()->config.set("extruder", 2);
}

struct SupportToolLayer
{
    double        z { 0. };
    std::set<int> tools;         // every tool extruding on the layer
    std::set<int> support_tools; // tools extruding Support / Support interface / Support transition
    std::set<int> base_tools;    // tools extruding Support (the base)
};

std::vector<SupportToolLayer> parse_support_tools(const std::string &gcode)
{
    std::vector<SupportToolLayer> out;
    std::istringstream            in(gcode);
    std::string                   line;
    int                           tool = 0;
    std::string                   type;
    while (std::getline(in, line)) {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind(";LAYER_CHANGE", 0) == 0) {
            out.emplace_back();
            continue;
        }
        if (line.rfind(";Z:", 0) == 0) {
            if (! out.empty())
                out.back().z = std::stod(line.substr(3));
            continue;
        }
        if (line.rfind(";TYPE:", 0) == 0) {
            type = line.substr(6);
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit((unsigned char) line[1])) {
            tool = std::stoi(line.substr(1));
            continue;
        }
        if (out.empty() || line.compare(0, 3, "G1 ") != 0 || line.find(" X") == std::string::npos)
            continue;
        const size_t e = line.find(" E");
        if (e == std::string::npos || e + 2 >= line.size() || line[e + 2] == '-')
            continue;
        out.back().tools.insert(tool);
        if (type.rfind("Support", 0) == 0)
            out.back().support_tools.insert(tool);
        if (type == "Support")
            out.back().base_tools.insert(tool);
    }
    return out;
}

const SupportToolLayer *gcode_layer_at(const std::vector<SupportToolLayer> &layers, double z)
{
    for (const SupportToolLayer &layer : layers)
        if (std::abs(layer.z - z) < 1e-3)
            return &layer;
    return nullptr;
}

struct LedgePlate
{
    Print                         print;
    std::string                   gcode;
    std::vector<SupportToolLayer> layers;
    const PrintObject            *ledge { nullptr };
};

void slice_ledge(LedgePlate &plate, const DynamicPrintConfig &config)
{
    Model model;
    add_ledge_plate(model);
    plate.print.apply(model, config);
    plate.print.validate();
    plate.print.set_status_silent();
    plate.gcode = Test::gcode(plate.print);
    REQUIRE(! plate.gcode.empty());
    plate.layers = parse_support_tools(plate.gcode);
    plate.ledge  = object_named(plate.print, "ledge");
    REQUIRE(plate.ledge != nullptr);
    REQUIRE(plate.ledge->support_layer_count() > 4);
}

bool has_role(const SupportLayer &layer, ExtrusionRole role)
{
    for (const PathInfo &path : support_paths(layer))
        if (path.role == role)
            return true;
    return false;
}

} // namespace


TEST_CASE("Adaptive layer merge support: don't care support base goes to a nozzle that can print its height", "[AdaptiveLayerMergeSupport]")
{
    // 0.12 layers: outside the 0.8's window, inside the 0.4's.
    // Support "don't care", interface on filament 1: process_layer sends the base to "a filament other than the
    // interface's", which on the block's slab tops is filament 2 (the same branch the merge plan's interface pin puts
    // a "don't care" interface layer in, review finding g). Its 0.8 cannot extrude the 0.12 base; the 0.4 can.
    const DynamicPrintConfig config = support_plate("0.4,0.8", "0.12", "0.08,0.16", "0.28,0.56", "0", "1", "normal(auto)", "default");
    LedgePlate plate;
    slice_ledge(plate, config);
    REQUIRE(plate.print.uses_multiple_nozzle_diameters());
    const PrintConfig &print_config = plate.print.config();
    REQUIRE(adaptive_layer_height_range_for_nozzle(print_config, 1, { 0, 1 }).contains(0.12));
    REQUIRE_FALSE(adaptive_layer_height_range_for_nozzle(print_config, 2, { 0, 1 }).contains(0.12));

    size_t exercised = 0;
    for (const SupportLayer *layer : plate.ledge->support_layers()) {
        if (layer->planned_filament_id != 0 || ! has_role(*layer, erSupportMaterial))
            continue;
        const SupportToolLayer *glayer = gcode_layer_at(plate.layers, layer->print_z);
        REQUIRE(glayer != nullptr);
        if (glayer->tools.count(1) == 0)
            continue;
        CAPTURE(layer->print_z);
        CHECK_THAT(layer->height, WithinAbs(0.12, 1e-6));
        // The base on T0, with the interface filament, instead of on the 0.8.
        CHECK(glayer->base_tools == std::set<int>{ 0 });
        ++ exercised;
    }
    // Layer 0 and the block's slab tops under the ledge object's support: every other layer up to about z 5.8.
    CHECK(exercised >= 5);

    // Nowhere on the plate does the 0.8 print the 0.4 object's support at a height it cannot extrude, with an
    // explicit interface filament or with both roles "don't care".
    for (const SupportToolLayer &glayer : plate.layers) {
        CAPTURE(glayer.z);
        CHECK(glayer.support_tools.count(1) == 0);
    }
    LedgePlate dontcare;
    slice_ledge(dontcare, support_plate("0.4,0.8", "0.12", "0.08,0.16", "0.28,0.56", "0", "0", "normal(auto)", "default"));
    for (const SupportToolLayer &glayer : dontcare.layers) {
        CAPTURE(glayer.z);
        CHECK(glayer.support_tools.count(1) == 0);
    }
}

TEST_CASE("Adaptive layer merge support: a single-diameter plate keeps its don't care support pick", "[AdaptiveLayerMergeSupport]")
{
    // The same plate on two 0.4 nozzles, extruder 2 still with min_layer_height 0.24: its per-nozzle window rejects
    // 0.2, but the check is off on a single-diameter plate, so the base keeps going to the filament other than the
    // interface's (T1, which prints the block on every layer), exactly as before the check.
    const DynamicPrintConfig config = support_plate("0.4,0.4", "0.2", "0.08,0.24", "0.28,0.28", "0", "1", "normal(auto)", "default");
    LedgePlate plate;
    slice_ledge(plate, config);
    REQUIRE_FALSE(plate.print.uses_multiple_nozzle_diameters());
    REQUIRE_FALSE(adaptive_layer_height_range_for_nozzle(plate.print.config(), 2, { 0, 1 }).contains(0.2));
    size_t base_layers = 0;
    for (const SupportLayer *layer : plate.ledge->support_layers()) {
        CHECK(layer->planned_filament_id == 0);
        CHECK(layer->planned_interface_filament_id == 0);
        if (! has_role(*layer, erSupportMaterial))
            continue;
        const SupportToolLayer *glayer = gcode_layer_at(plate.layers, layer->print_z);
        REQUIRE(glayer != nullptr);
        CAPTURE(layer->print_z);
        CHECK(glayer->base_tools == std::set<int>{ 1 });
        ++ base_layers;
    }
    CHECK(base_layers >= 10);
}
