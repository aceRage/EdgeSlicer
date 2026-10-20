// Tree support smoke tests and run-to-run determinism.
//
// Ported from OrcaSlicer #15575 (the four basic cases) and #15565 ("Make Tree-Support
// Deterministic", the two [Regression] cases), converted to Catch2 v2 and to the helpers of this
// tree. The thread-count cases live in test_tree_support_determinism.cpp.
//
// Note that this fork already removed the scheduling dependence of drop_nodes() and of the minimum
// spanning tree on its own (the plan / apply split in TreeSupport::drop_nodes(), commits 369ab534796
// and cbb1f0d791); #15565 adds the coordinate tie-break of the spanning tree and the fixed bucket
// size of the organic influence area merge, which are the parts these cases pin down.

#include <catch2/catch.hpp>

#include <algorithm>
#include <initializer_list>

#include <tbb/global_control.h>

#include "libslic3r/Config.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Support/TreeSupportCommon.hpp"
#include "libslic3r/libslic3r.h"

#include "test_data.hpp"

using namespace Slic3r::Test;
using namespace Slic3r;

namespace {

// The upper plate overhangs both the lower plate and open air, so branches land on the model and on
// the bed in the same slice.
TriangleMesh two_tier_mesh()
{
    TriangleMesh lower  = make_cube(30, 30, 3);
    TriangleMesh column = make_cube(8, 8, 15);
    TriangleMesh upper  = make_cube(50, 50, 3);
    // Each part overlaps the one below rather than resting on it; a coplanar join slices ambiguously.
    column.translate(11.f, 11.f, 2.f);
    upper.translate(-10.f, -10.f, 16.f);
    TriangleMesh mesh = lower;
    mesh.merge(column);
    mesh.merge(upper);
    return mesh;
}

TriangleMesh scaled(TestMesh id, float scale)
{
    TriangleMesh mesh = Slic3r::Test::mesh(id);
    mesh.scale(scale);
    return mesh;
}

// `extra` is applied last, so a caller can add or override any key.
void slice_with_tree_support(const TriangleMesh &mesh, Slic3r::Print &print, const char *style,
                             int threshold_angle = 30, int build_plate_only = 0, int raft_layers = 0,
                             std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",              1 },
        { "support_type",                "tree(auto)" },
        { "support_style",               style },
        { "support_on_build_plate_only", build_plate_only },
        { "support_threshold_angle",     threshold_angle },
        { "raft_layers",                 raft_layers },
        { "layer_height",                0.2 },
    });
    config.set_deserialize_strict(extra);
    Slic3r::Test::init_and_process_print({ mesh }, print, config);
}

Points support_points(const Slic3r::Print &print)
{
    Points points;
    for (const SupportLayer *layer : print.objects().front()->support_layers())
        layer->support_fills.collect_points(points);
    return points;
}

size_t support_point_count(const TriangleMesh &mesh, const char *style, int threshold_angle = 30,
                           int build_plate_only = 0)
{
    Slic3r::Print print;
    slice_with_tree_support(mesh, print, style, threshold_angle, build_plate_only);
    return support_points(print).size();
}

// Index of the first differing point, or the common length when they match. An index keeps a
// failure readable; comparing the vectors themselves dumps thousands of points.
size_t first_difference(const Points &a, const Points &b)
{
    const size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i)
        if (a[i] != b[i])
            return i;
    return common;
}

// Slice `mesh` twice and require an identical support point sequence. Point counts and total
// length are order insensitive, so the sequence is what a reordering shows up in.
void sliced_twice_matches(const TriangleMesh &mesh, int build_plate_only, const char *style = "tree_slim",
                          std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra = {})
{
    Slic3r::Print first_print, second_print;
    slice_with_tree_support(mesh, first_print, style, 30, build_plate_only, 0, extra);
    slice_with_tree_support(mesh, second_print, style, 30, build_plate_only, 0, extra);
    const Points first  = support_points(first_print);
    const Points second = support_points(second_print);
    REQUIRE(first.size() > 1000); // without support the comparison below passes vacuously
    REQUIRE(second.size() == first.size());
    REQUIRE(first_difference(first, second) == first.size());
}

} // namespace

TEST_CASE("Tree support is generated for an overhang and not for a plain cube", "[TreeSupport]")
{
    REQUIRE(support_point_count(scaled(TestMesh::overhang, 2.f), "tree_slim") > 1000);
    REQUIRE(support_point_count(Slic3r::Test::mesh(TestMesh::cube_20x20x20), "tree_slim") == 0);
}

TEST_CASE("Restricting tree support to the build plate changes what is generated", "[TreeSupport]")
{
    const TriangleMesh mesh = two_tier_mesh();
    const size_t anywhere    = support_point_count(mesh, "tree_slim", 30, 0);
    const size_t plate_only  = support_point_count(mesh, "tree_slim", 30, 1);
    REQUIRE(anywhere > 1000);
    REQUIRE(plate_only > 1000);
    // The upper plate overhangs the lower one, so some branches would land on the model.
    REQUIRE(plate_only != anywhere);
}

TEST_CASE("Tree support layers rise monotonically within the layer height limits", "[TreeSupport]")
{
    Slic3r::Print print;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), print, "tree_slim");
    const double nozzle = print.config().nozzle_diameter.values.front();

    size_t checked = 0;
    double previous = 0;
    bool   previous_was_adjacent = false;
    for (const SupportLayer *layer : print.objects().front()->support_layers()) {
        if (layer->print_z <= 0 || layer->height <= 0) {
            // Layers with no nodes are left at zero. Skipping one leaves a hole, so the next pair
            // spans more than one layer and its gap says nothing about the layer height limit.
            previous_was_adjacent = false;
            continue;
        }
        if (previous > 0) {
            CAPTURE(previous, layer->print_z);
            REQUIRE(layer->print_z > previous);
            if (previous_was_adjacent)
                REQUIRE(layer->print_z - previous <= nozzle + EPSILON);
        }
        previous = layer->print_z;
        previous_was_adjacent = true;
        ++checked;
    }
    REQUIRE(checked > 10);
}

TEST_CASE("A raft is still generated under tree support", "[TreeSupport]")
{
    // The mesh supports itself, so a layer count alone passes with no raft at all.
    Slic3r::Print rafted, unrafted;
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), rafted, "tree_slim", 30, 0, 3);
    slice_with_tree_support(scaled(TestMesh::overhang, 2.f), unrafted, "tree_slim", 30, 0, 0);
    const PrintObject *rafted_object   = rafted.objects().front();
    const PrintObject *unrafted_object = unrafted.objects().front();
    REQUIRE(rafted_object->support_layers().size() > unrafted_object->support_layers().size());
    // The raft goes under the object.
    REQUIRE(rafted_object->layers().front()->print_z > unrafted_object->layers().front()->print_z);
}

// drop_nodes() decides the node merges and spawns the next layer's nodes in parallel. Every one of
// those decisions has to be applied in a fixed order, or the same model gives different branches on
// each slice.
TEST_CASE("Tree support toolpaths do not depend on thread scheduling", "[TreeSupport][Regression]")
{
    // Scaled up so that a layer holds enough nodes for the parallel range to be split. At stock
    // size it stays in one chunk and the order never varies.
    SECTION("overhang")            { sliced_twice_matches(scaled(TestMesh::overhang, 2.f), 0); }
    SECTION("bridge with hole")    { sliced_twice_matches(scaled(TestMesh::bridge_with_hole, 3.f), 0); }
    // Dropping every branch that cannot reach the bed leaves the survivors dense enough that the
    // neighbour merge fires in bulk.
    SECTION("on the build plate")  { sliced_twice_matches(scaled(TestMesh::overhang, 4.f), 1); }
    // Branches resting on the model are what put nodes in a part group other than 0, which is the
    // only way to reach the prune in the second pass. tree_hybrid additionally builds polygon
    // nodes, so it is the only style that exercises the overhang merge.
    SECTION("resting on the model") { sliced_twice_matches(two_tier_mesh(), 0); }
    SECTION("hybrid on the model")  { sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid"); }
}

// Prim breaks equal-distance ties by heap address. A 1 mm branch diameter puts neighbours close
// enough to tie, and an explicit line width pins max_move_dist, so the moved tie winner reaches
// the support toolpaths.
TEST_CASE("Tree support toolpaths do not depend on the MST tie order", "[TreeSupport][Regression]")
{
    sliced_twice_matches(two_tier_mesh(), 0, "tree_hybrid", {
        { "tree_support_branch_diameter", 1.0 },
        { "tree_support_branch_distance", 5.0 },
        { "tree_support_branch_angle",    40 },
        { "support_line_width",           0.4 },
    });
}

TEST_CASE("Organic tree support toolpaths do not depend on the run", "[TreeSupport][Regression]")
{
    sliced_twice_matches(scaled(TestMesh::overhang, 2.f), 0, "organic");
    sliced_twice_matches(two_tier_mesh(), 0, "organic");
}

// The organic generator merges influence areas in buckets. The bucket size used to depend on
// max_concurrency(), so the same model gave different branches on a machine with more cores. A small
// model keeps every layer under 4 * max_concurrency() influence areas, which is where the old
// rule switched to 2-element buckets.
TEST_CASE("Organic tree support toolpaths do not depend on the thread count", "[TreeSupport][Regression]")
{
    auto points_with_threads = [](size_t max_threads) {
        tbb::global_control gc(tbb::global_control::max_allowed_parallelism, max_threads);
        Slic3r::Print print;
        slice_with_tree_support(two_tier_mesh(), print, "organic");
        return support_points(print);
    };
    const Points one = points_with_threads(1);
    REQUIRE(one.size() > 1000);
    for (size_t threads : { size_t(2), size_t(8), size_t(32) }) {
        INFO("max_allowed_parallelism = " << threads);
        const Points many = points_with_threads(threads);
        REQUIRE(many.size() == one.size());
        REQUIRE(first_difference(many, one) == one.size());
    }
}

// TreeSupportSettings::zero_top_z_gap is a process-global static that a zero top Z distance sets and
// that widens the minimum XY distance of every organic tree built while it is set. It was never
// reset, so once one slice of the process had a zero gap, every later slice was built as if it had
// one too: the same project sliced differently depending on what had been sliced before it (Orca
// #13936 resets it per pass). The small XY distance is what makes the widening visible.
TEST_CASE("Organic tree support: a zero top Z distance does not leak into the next slice", "[TreeSupport][Regression]")
{
    using Slic3r::TreeSupport3D::TreeSupportSettings;
    const TriangleMesh mesh = two_tier_mesh();
    auto slice = [&mesh](Slic3r::Print &print, double top_z_distance) {
        slice_with_tree_support(mesh, print, "organic", 30, 0, 0, {
            { "support_top_z_distance",     top_z_distance },
            { "support_object_xy_distance", 0.05 },
        });
    };
    // What a fresh process starts with.
    TreeSupportSettings::zero_top_z_gap = false;
    Slic3r::Print before;
    slice(before, 0.2);
    CHECK(! TreeSupportSettings::zero_top_z_gap);

    Slic3r::Print zero_gap;
    slice(zero_gap, 0.);
    CHECK(TreeSupportSettings::zero_top_z_gap);

    Slic3r::Print after;
    slice(after, 0.2);
    CHECK(! TreeSupportSettings::zero_top_z_gap);

    const Points first  = support_points(before);
    const Points second = support_points(after);
    REQUIRE(first.size() > 1000);
    REQUIRE(second.size() == first.size());
    REQUIRE(first_difference(first, second) == first.size());
}

// Snapmaker #995: tree (manual) support grows only where the user painted or placed an enforcer.
// Snapmaker's port of Bambu c03e13340 (STUDIO-11484) ran the full-layer automatic overhang detection on
// every layer that had an enforcer, so one painted corner put supports under every overhang of that
// layer. This tree never took that port: in manual mode TreeSupport::detect_overhangs() skips the
// automatic detection above enforce_support_layers and appends the enforced overhangs on their own. The
// case pins that, so a later port of the transition-layer rework (Snapmaker #937) cannot bring the leak in.
namespace {
// A T: a 10 mm column under a 50 mm slab whose two 20 mm arms overhang open air on either side.
TriangleMesh t_mesh()
{
    TriangleMesh column = make_cube(10, 10, 15);
    TriangleMesh slab   = make_cube(50, 10, 3);
    column.translate(20.f, 0.f, 0.f);
    slab.translate(0.f, 0.f, 14.f);
    TriangleMesh mesh = column;
    mesh.merge(slab);
    return mesh;
}

// Support points of the T with an enforcer under its left arm only, and the X of the column centre
// in the same (object) coordinates.
Points t_support_points(const char *support_type, const char *style, coord_t &column_center_x)
{
    Slic3r::Model model;
    ModelObject  *object = model.add_object();
    object->name = "t.stl";
    object->add_volume(t_mesh());
    TriangleMesh enforcer = make_cube(22, 12, 6);
    enforcer.translate(-1.f, -1.f, 12.f);
    object->add_volume(std::move(enforcer), ModelVolumeType::SUPPORT_ENFORCER);
    object->add_instance();
    object->instances.front()->set_offset(Vec3d(100., 100., 0.));
    object->ensure_on_bed();

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",          1 },
        { "support_type",            support_type },
        { "support_style",           style },
        { "support_threshold_angle", 30 },
        { "layer_height",            0.2 },
    });
    Slic3r::Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    print.process();

    const PrintObject *print_object = print.objects().front();
    REQUIRE(print_object->layer_count() > 20);
    // Layer 10 (about 2 mm up) is the column alone.
    column_center_x = get_extents(print_object->get_layer(10)->lslices).center().x();
    return support_points(print);
}

size_t points_right_of(const Points &points, coord_t x)
{
    return size_t(std::count_if(points.begin(), points.end(), [x](const Point &p) { return p.x() > x; }));
}
} // namespace

TEST_CASE("Tree (manual) support grows only under the enforced overhang", "[TreeSupport][Snap995]")
{
    // The right arm starts 5 mm right of the column centre; nothing enforced reaches past it.
    for (const char *style : { "tree_slim", "organic" }) {
        INFO("support_style = " << style);
        coord_t center_auto = 0, center_manual = 0;
        const Points automatic = t_support_points("tree(auto)", style, center_auto);
        const Points manual    = t_support_points("tree(manual)", style, center_manual);
        // Automatic support holds up both arms, so the right-hand check below is not vacuous.
        REQUIRE(points_right_of(automatic, center_auto + scaled<coord_t>(6.)) > 100);
        // Manual support exists under the enforced left arm...
        REQUIRE(manual.size() > 100);
        // ...and nowhere under the unpainted right arm.
        CHECK(points_right_of(manual, center_manual + scaled<coord_t>(6.)) == 0);
    }
}
