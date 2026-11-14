// Organic tree support: the bottom Z gap under a branch that is carried down onto the part.

#include <catch2/catch.hpp>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

constexpr double LAYER_HEIGHT = 0.2;
// How many part layers below a foot are searched for the part.
constexpr int    MAX_GAP_LAYERS = 6;

indexed_triangle_set box_its(double x0, double y0, double z0, double dx, double dy, double dz)
{
    indexed_triangle_set its = its_make_cube(dx, dy, dz);
    for (stl_vertex &v : its.vertices)
        v += Vec3f(float(x0), float(y0), float(z0));
    return its;
}

// A ramp on a plinth, rising in +x by the given slope, a pillar at its top and a ledge on the pillar
// reaching back over the whole ramp: organic branches under the ledge come down on the slope, away
// from any placeable area, and are carried down onto it. The plinth keeps them off the bed.
indexed_triangle_set ledge_over_ramp_its(double slope_deg)
{
    const float X0 = 20.f, Y0 = 20.f, Z0 = 4.f, L = 20.f, W = 20.f;
    const float H = float(L * std::tan(slope_deg * M_PI / 180.));
    indexed_triangle_set ramp;
    ramp.vertices = { { 0, 0, 0 }, { L, 0, 0 }, { L, 0, H }, { 0, W, 0 }, { L, W, 0 }, { L, W, H } };
    ramp.indices  = { { 0, 4, 1 }, { 0, 3, 4 },   // bottom
                      { 1, 4, 5 }, { 1, 5, 2 },   // x = L
                      { 0, 2, 5 }, { 0, 5, 3 },   // slope
                      { 0, 1, 2 },                // y = 0
                      { 3, 5, 4 } };              // y = W
    for (stl_vertex &v : ramp.vertices)
        v += Vec3f(X0, Y0, Z0 - 0.5f);
    const double top = Z0 + H + 8.;
    indexed_triangle_set its = box_its(0., 0., 0., X0 + L + 25., Y0 + W + 20., Z0);
    its_merge(its, ramp);
    its_merge(its, box_its(X0 + L - 0.5, Y0, Z0 - 0.5, 4.5, W, top - Z0 + 0.5));
    its_merge(its, box_its(X0 + 2., Y0, top - 2., L + 2.5, W, 2.));
    return its;
}

struct FootGap
{
    // Support line length (mm) of the lowest support layer at each spot, by the number of empty
    // layers between it and the part right under it; index MAX_GAP_LAYERS + 1: no part within reach.
    std::vector<double> by_gap = std::vector<double>(MAX_GAP_LAYERS + 2, 0.);
    // The part of by_gap standing on a level top surface: no part of the next layer up within 1 mm.
    std::vector<double> by_gap_level = std::vector<double>(MAX_GAP_LAYERS + 1, 0.);
    double              total_length = 0.;
    // Support line length inside the part's own layer.
    double              in_part = 0.;
    // Support line length with no part anywhere below it (standing on the bed).
    double              over_bed = 0.;

    double at(int gap_layers) const { return by_gap[gap_layers]; }
    double on_part() const { double s = 0.; for (int i = 0; i <= MAX_GAP_LAYERS; ++ i) s += by_gap[i]; return s; }
    std::string str() const
    {
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << "lines " << total_length << " mm, over the bed " << over_bed << ", in part " << in_part << ", feet by gap:";
        for (int i = 0; i <= MAX_GAP_LAYERS; ++ i)
            ss << " " << std::setprecision(1) << i * LAYER_HEIGHT << ":" << by_gap[i];
        ss << " none:" << by_gap[MAX_GAP_LAYERS + 1] << "; of which on level surfaces:";
        for (int i = 0; i <= MAX_GAP_LAYERS; ++ i)
            ss << " " << std::setprecision(1) << i * LAYER_HEIGHT << ":" << by_gap_level[i];
        return ss.str();
    }
};

Polylines support_lines(const SupportLayer &support_layer)
{
    Polylines lines = support_layer.support_fills.as_polylines();
    for (const auto &by_extruder : support_layer.interface_by_extruder)
        append(lines, by_extruder.second.as_polylines());
    return lines;
}

double length_mm(const Polylines &lines)
{
    double l = 0.;
    for (const Polyline &pl : lines)
        l += unscale<double>(pl.length());
    return l;
}

// For each support layer, the lines with no support of the layer below under them are a foot's
// lowest layer. Each piece is matched with the highest part layer under it (0.05 mm inside
// the part's outline) and counted at the number of layers left empty in between.
FootGap measure_foot_gap(const Print &print)
{
    FootGap out;
    for (const PrintObject *object : print.objects()) {
        std::map<long, int> layer_of_z;
        for (const Layer *layer : object->layers())
            layer_of_z[lround(layer->print_z * 1000.)] = int(layer->id());
        std::vector<ExPolygons> inner(object->layers().size()), level(object->layers().size());
        for (const Layer *layer : object->layers())
            inner[layer->id()] = offset_ex(layer->lslices, -scaled<float>(0.05));
        for (const Layer *layer : object->layers())
            level[layer->id()] = layer->upper_layer ? diff_ex(inner[layer->id()], offset(layer->upper_layer->lslices, scaled<float>(1.))) : inner[layer->id()];
        Polygons support_below;
        Polygons part_below; // the union of the part's layers below the current one
        int      part_below_layers = 0;
        int      support_below_layer = -100;
        for (const SupportLayer *support_layer : object->support_layers()) {
            Polylines lines = support_lines(*support_layer);
            out.total_length += length_mm(lines);
            auto it = layer_of_z.find(lround(support_layer->print_z * 1000.));
            REQUIRE(it != layer_of_z.end());
            const int layer_id = it->second;
            out.in_part += length_mm(intersection_pl(lines, inner[layer_id]));
            for (; part_below_layers < layer_id; ++ part_below_layers)
                part_below = union_(part_below, to_polygons(object->get_layer(part_below_layers)->lslices));
            out.over_bed += length_mm(diff_pl(lines, part_below));
            Polylines feet = support_below_layer == layer_id - 1 ? diff_pl(lines, support_below) : lines;
            // The area the layer's lines cover, the gaps between the lines of a dense (interface)
            // fill closed: a line over them is not a foot.
            support_below       = closing(offset(lines, scaled<float>(0.25)), scaled<float>(0.5));
            support_below_layer = layer_id;
            for (int gap = 0; gap <= MAX_GAP_LAYERS && ! feet.empty(); ++ gap) {
                const int below = layer_id - 1 - gap;
                if (below < 0)
                    break;
                out.by_gap[gap] += length_mm(intersection_pl(feet, inner[below]));
                out.by_gap_level[gap] += length_mm(intersection_pl(feet, level[below]));
                feet = diff_pl(feet, inner[below]);
            }
            out.by_gap[MAX_GAP_LAYERS + 1] += length_mm(feet);
        }
    }
    return out;
}

DynamicPrintConfig organic_config(int bottom_interface_layers, double bottom_z_distance)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", 1 },
        { "support_type", "tree(auto)" },
        { "support_style", "organic" },
        { "support_on_build_plate_only", 0 },
        { "support_threshold_angle", 30 },
        { "support_top_z_distance", 0.2 },
        { "layer_height", LAYER_HEIGHT },
        { "initial_layer_print_height", LAYER_HEIGHT },
        { "wall_generator", "classic" },
    });
    config.set_key_value("support_bottom_z_distance", new ConfigOptionFloat(bottom_z_distance));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(bottom_interface_layers));
    return config;
}

FootGap slice_ramp(double slope_deg, const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    print.set_status_silent();
    init_print({ TriangleMesh(ledge_over_ramp_its(slope_deg)) }, print, model, config);
    for (ModelObject *object : model.objects)
        for (ModelInstance *instance : object->instances)
            instance->set_offset(instance->get_offset() + Vec3d(90., 90., 0.));
    print.apply(model, config);
    print.process();
    return measure_foot_gap(print);
}

FootGap slice_benchy(const DynamicPrintConfig &config)
{
    Model model;
    {
        DynamicPrintConfig        cfg;
        ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::EnableSilent);
        PlateDataPtrs             plate_data;
        std::vector<Preset *>     presets;
        bool                      is_bbl = false;
        Semver                    version;
        const std::string path = std::string(TEST_DATA_DIR) + "/../../resources/handy_models/3DBenchy.3mf";
        REQUIRE(load_bbs_3mf(path.c_str(), &cfg, &ctxt, &model, &plate_data, &presets, &is_bbl, &version, nullptr, LoadStrategy::LoadModel));
        release_PlateData_list(plate_data);
        REQUIRE(! model.objects.empty());
        for (ModelObject *object : model.objects)
            object->ensure_on_bed();
    }
    Print print;
    print.set_status_silent();
    for (ModelObject *object : model.objects)
        print.auto_assign_extruders(object);
    print.apply(model, config);
    print.process();
    return measure_foot_gap(print);
}

void report(const std::string &what, const FootGap &gap)
{
    if (std::getenv("ORGANIC_FOOT_GAP_REPORT"))
        std::cout << "FOOTGAP " << what << " | " << gap.str() << std::endl;
}

} // namespace

// In organic_draw_branches() a branch that rests on the part away from a placeable area (on a
// slope) is carried a few layers further down, each layer cut by a collision of the part. The
// collision of layer L holds the part from L - z_distance_bottom_layers to L, so a layer cut by its
// own collision keeps the configured bottom gap; a layer cut by the collision of the layer above
// keeps one layer less. With bottom interface layers the branch also gets a bottom contact, which
// is not printed (it only seeds the interface layers above it): on the foot's own layer it lifts
// the foot by a layer. The foot's lowest printed layer has to keep the configured bottom Z gap, no
// less (support fused to the part) and no more, with and without bottom interface layers.
TEST_CASE("Organic tree support feet carried down keep the configured bottom Z gap", "[TreeSupport][OrganicFootGap]")
{
    for (const int bottom_interface_layers : { 0, 2 })
        for (const double bottom_z : { 0.2, 0.4 }) {
            const int gap_layers = int(std::lround(bottom_z / LAYER_HEIGHT));
            const DynamicPrintConfig config = organic_config(bottom_interface_layers, bottom_z);
            std::ostringstream name;
            name << "interface bottom layers " << bottom_interface_layers << ", bottom Z " << bottom_z;
            const std::vector<std::pair<std::string, FootGap>> parts {
                { "ramp 35", slice_ramp(35., config) },
                { "ramp 45", slice_ramp(45., config) },
                { "benchy", slice_benchy(config) },
            };
            for (const auto &[part, gap] : parts) {
                report(part + ", " + name.str(), gap);
                INFO(part << ", " << name.str() << ": " << gap.str());
                REQUIRE(gap.total_length > 1000.);
                CHECK(gap.in_part == 0.);
                // No support closer to the part than the gap.
                for (int i = 0; i < gap_layers; ++ i)
                    CHECK(gap.at(i) == 0.);
                // Most of what stands on the part stands at the gap, not at a gap a layer larger
                // (a slope leaves some feet higher, by the XY distance and where a branch stops).
                CHECK(gap.at(gap_layers) > 2. * gap.at(gap_layers + 1));
                CHECK(gap.at(gap_layers) > 0.6 * gap.on_part());
            }
        }
}
