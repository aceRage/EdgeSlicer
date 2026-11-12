// Tree support errors found by the Rust port (fix/tree-support-errors).

#include <catch2/catch.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

indexed_triangle_set box_its(double x0, double y0, double z0, double dx, double dy, double dz)
{
    indexed_triangle_set its = its_make_cube(dx, dy, dz);
    for (stl_vertex &v : its.vertices)
        v += Vec3f(float(x0), float(y0), float(z0));
    return its;
}

double area_mm2(const ExPolygons &expolys)
{
    double a = 0.;
    for (const ExPolygon &e : expolys)
        a += e.area();
    return a * SCALING_FACTOR * SCALING_FACTOR;
}

// init_print() arranges the object around the origin, half of it off the bed, where the supports are
// cut away: move it to the middle of the default 200 x 200 mm bed.
void slice_on_bed(const indexed_triangle_set &its, Print &print, Model &model, const DynamicPrintConfig &config)
{
    init_print({ TriangleMesh(its) }, print, model, config);
    for (ModelObject *object : model.objects)
        for (ModelInstance *instance : object->instances)
            instance->set_offset(instance->get_offset() + Vec3d(100., 100., 0.));
    print.apply(model, config);
    print.process();
}

} // namespace

// TreeSupport::detect_overhangs() groups the overhangs into clusters, one per layer and cluster
// (a std::map keyed by layer). A second overhang of the same layer that joined a cluster was
// added to the cluster's outline but not to its overhangs, and only those become the layer's
// overhangs (Layer::loverhangs): it got no support at all.
//
// Here a 10 x 10 mm block carries ten layers that each reach 1 mm further out in +y than the one
// below, split down the middle by a 0.2 mm slot. Every layer has two overhangs, one each side of the
// slot, close enough to join the same cluster: the right one was dropped on every layer but the
// first.
TEST_CASE("Tree support keeps every overhang of a layer that joins a cluster", "[TreeSupport][TreeSupportFixes]")
{
    indexed_triangle_set its = box_its(0., 0., 0., 10., 10., 2.);
    const int    steps = 10;
    const double lh    = 0.2;
    for (int k = 1; k <= steps; ++k) {
        const double z = 2. + lh * (k - 1), depth = 10. + k;
        its_merge(its, box_its(0., 0., z, 4.9, depth, lh));  // left of the slot
        its_merge(its, box_its(5.1, 0., z, 4.9, depth, lh)); // right of the slot
    }

    for (const char *style : { "tree_slim", "organic" }) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", 1 },
            { "support_type", "tree(auto)" },
            { "support_style", style },
            // A steep threshold: the layer below is grown by 0.2 / tan(70) = 0.07 mm, so each
            // overhang is nearly the whole 1 mm step and sits right next to the other side's.
            { "support_threshold_angle", 70 },
            { "layer_height", lh },
            { "initial_layer_print_height", lh },
            { "max_bridge_length", 0 },
            // The default 0 makes the cluster test's reach (one line width) nothing; profiles set it.
            { "line_width", 0.42 },
        });
        Print print;
        Model model;
        slice_on_bed(its, print, model, config);
        const PrintObject &object = *print.objects().front();

        // The slot is at the middle of the object in x.
        const BoundingBox bbox = get_extents(object.layers().front()->lslices);
        const coord_t     mid  = bbox.center().x();
        const Polygon     left_half({ { bbox.min.x() - scaled<coord_t>(50.), bbox.min.y() - scaled<coord_t>(50.) },
                                      { mid, bbox.min.y() - scaled<coord_t>(50.) },
                                      { mid, bbox.max.y() + scaled<coord_t>(50.) },
                                      { bbox.min.x() - scaled<coord_t>(50.), bbox.max.y() + scaled<coord_t>(50.) } });
        int left = 0, right = 0, stepped = 0;
        for (const Layer *layer : object.layers()) {
            // The stepped layers above the first one.
            if (layer->print_z < 2. + 1.5 * lh - EPSILON || layer->print_z > 2. + steps * lh + EPSILON)
                continue;
            ++stepped;
            if (area_mm2(intersection_ex(layer->support_overhangs, ExPolygons{ ExPolygon(left_half) })) > 1.)
                ++left;
            if (area_mm2(diff_ex(layer->support_overhangs, ExPolygons{ ExPolygon(left_half) })) > 1.)
                ++right;
        }
        INFO(style << ": " << stepped << " stepped layers, " << left << " with an overhang left of the slot, " << right << " right of it");
        REQUIRE(stepped == steps - 1);
        CHECK(left == stepped);
        CHECK(right == stepped);
    }
}

// What that cost: a 10 x 10 x 5 mm pillar carries a 0.2 mm plate reaching 6 mm out in +y, and on it a
// part split by a 0.2 mm slot reaching 6 mm further. The plate's overhang is one piece; the part's two
// overhangs (one each side of the slot) both join its cluster, and the second was dropped: the half
// of the part on that side got no support under it.
TEST_CASE("Tree support holds up both overhangs that join a cluster on the same layer", "[TreeSupport][TreeSupportFixes]")
{
    const double lh = 0.2;
    indexed_triangle_set its = box_its(0., 0., 0., 10., 10., 5.);
    its_merge(its, box_its(0., 0., 5., 10., 16., lh));        // the plate
    its_merge(its, box_its(0., 0., 5. + lh, 4.9, 22., 0.6));  // left of the slot
    its_merge(its, box_its(5.1, 0., 5. + lh, 4.9, 22., 0.6)); // right of the slot

    for (const char *style : { "tree_slim", "organic" }) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", 1 },
            { "support_type", "tree(auto)" },
            { "support_style", style },
            { "support_threshold_angle", 30 },
            { "layer_height", lh },
            { "initial_layer_print_height", lh },
            { "max_bridge_length", 0 },
            { "line_width", 0.42 },
        });
        Print print;
        Model model;
        slice_on_bed(its, print, model, config);
        const PrintObject &object = *print.objects().front();

        // Object coordinates: the model's (0, 0) is at the first layer's minimum corner.
        const BoundingBox bbox   = get_extents(object.layers().front()->lslices);
        const Point       origin = bbox.min;
        auto rect = [&origin](double x0, double y0, double x1, double y1) {
            return ExPolygons{ ExPolygon(Polygon({ origin + Point(scaled<coord_t>(x0), scaled<coord_t>(y0)), origin + Point(scaled<coord_t>(x1), scaled<coord_t>(y0)),
                                                   origin + Point(scaled<coord_t>(x1), scaled<coord_t>(y1)), origin + Point(scaled<coord_t>(x0), scaled<coord_t>(y1)) })) };
        };
        // The two overhangs of the split part, 6 mm deep, less a line width at each edge.
        const ExPolygons left_overhang  = rect(0.5, 16.5, 4.4, 21.5);
        const ExPolygons right_overhang = rect(5.6, 16.5, 9.5, 21.5);

        // The support lines of the layers below the split part, within the top Z gap and a layer.
        const double z_part = 5. + 2. * lh;
        Polygons     cover;
        for (const SupportLayer *layer : object.support_layers())
            if (layer->print_z < z_part - lh - EPSILON && layer->print_z > z_part - 4. * lh) {
                Polylines lines = layer->support_fills.as_polylines();
                for (const auto &by_extruder : layer->interface_by_extruder)
                    append(lines, by_extruder.second.as_polylines());
                append(cover, offset(lines, scaled<float>(0.25)));
            }
        const ExPolygons covered = union_ex(cover);
        // Support layers are in the same coordinates as the object's layers.
        const double left  = area_mm2(intersection_ex(left_overhang, covered)) / area_mm2(left_overhang);
        const double right = area_mm2(intersection_ex(right_overhang, covered)) / area_mm2(right_overhang);
        INFO(style << ": " << left << " of the left overhang and " << right << " of the right one have support lines under them");
        CHECK(left > 0.5);
        CHECK(right > 0.5);
    }
}
