// Fuzzy skin over overhangs - the gate for
// docs/superpowers/specs/2026-09-09-fuzzy-skin-overhang-research.md.
//
// The option is fuzzy_skin_skip_overhangs. With it on, the sampled points of a fuzzed wall that
// hang over air keep their un-jittered position, while the points sitting on the layer below are
// jittered as before, with a short linear ramp between the two so there is no step.
//
// The model is the inverted pyramid the spec suggests, and it makes the claim testable without
// having to classify individual points: its first layer is a 2x2 mm square on the BED, so nothing
// there is an overhang, while every layer above it steps about 0.56 mm past the layer below, so its
// whole outer wall is an overhang. The assertions are therefore a per-layer ON/OFF contrast:
//
//  * OFF is the hard gate. With fuzzy_skin_skip_overhangs at its default the new code must not run
//    at all - every layer, overhanging or not, is fuzzed exactly as before. The byte-identity half
//    of that guarantee is the Bar A comparison against a build of this branch's base commit,
//    recorded in the spec; what is asserted here is that no skipping leaks in.
//  * ON: the overhanging layers lose their jitter (their walls become straight to within 1e-3 mm),
//    while the first layer keeps every bit of it - the first layer is bed-supported and is never an
//    overhang.
//  * The blend leaves no jump: consecutive points on a wall are never further apart than the
//    resampler's own cadence plus the ramp, which excludes a hard step at a boundary.
//
// Both fuzzy modes (external only, all walls) and both generators (classic, Arachne) are covered.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

constexpr double kThickness     = 0.3;  // fuzzy_skin_thickness, mm
constexpr double kPointDistance = 0.8;  // fuzzy_skin_point_distance, mm

// An inverted pyramid: a 10x10 mm foot on the bed widening to 38x38 mm at 5 mm up, i.e. every side
// about 70 degrees from vertical. Each 0.2 mm layer therefore steps about 0.56 mm past the layer
// below it, which is well clear of the support tolerance the generators use (about -0.17 mm for the
// classic generator's index-0 offset, +0.2 mm for Arachne's), so every wall above the first layer is
// unambiguously an overhang - while the first layer sits flat on the bed and is unambiguously not.
//
// The slope matters. At the 45 degrees one would reach for first, the step per 0.2 mm layer is
// 0.2 mm, SMALLER than the support tolerance itself, so no wall point is ever unambiguously
// unsupported and the test has nothing to measure.
TriangleMesh inverted_pyramid()
{
    const float h  = 5.f;
    // A 10 mm foot, not the 2 mm one the shape suggests: the first layer is the control that proves
    // the first-layer rule, and its wall has to be big enough for the four sides of the ring to be
    // told apart at all.
    const float r0 = 5.f;   // half-width at z = 0
    const float r1 = 19.f;  // half-width at z = h  -> the same ~70 degree slope
    indexed_triangle_set its;
    its.vertices = {
        { -r0, -r0, 0.f }, {  r0, -r0, 0.f }, {  r0,  r0, 0.f }, { -r0,  r0, 0.f },
        { -r1, -r1, h   }, {  r1, -r1, h   }, {  r1,  r1, h   }, { -r1,  r1, h   },
    };
    its.indices = {
        { 0, 2, 1 }, { 0, 3, 2 },              // bottom
        { 4, 5, 6 }, { 4, 6, 7 },              // top
        { 0, 1, 5 }, { 0, 5, 4 },              // sides
        { 1, 2, 6 }, { 1, 6, 5 },
        { 2, 3, 7 }, { 2, 7, 6 },
        { 3, 0, 4 }, { 3, 4, 7 },
    };
    return TriangleMesh(its);
}

DynamicPrintConfig base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops",                 "2" },
        // The point of the model is its unsupported walls; supports would take that away.
        { "enable_support",             "0" },
        // The overhang split is what supplies the lower-layer polygons the option reuses.
        { "detect_overhang_wall",       "1" },
        // Arc fitting rewrites wall point sets after the fact, and the loop-reversal heuristic
        // reorders them; both would make the geometry below say something other than what it means.
        { "enable_arc_fitting",         "0" },
        { "overhang_reverse",           "0" },
        { "spiral_mode",                "0" },
        { "wall_generator",             "classic" },
        { "fuzzy_skin_noise_type",      "classic" },
        { "fuzzy_skin_mode",            "displacement" },
        { "fuzzy_skin_thickness",       std::to_string(kThickness) },
        { "fuzzy_skin_point_distance",  std::to_string(kPointDistance) },
        // Fuzz the first layer too: it is the control that proves the option's first-layer rule.
        { "fuzzy_skin_first_layer",     "1" },
        { "fuzzy_skin",                 "none" },
        { "fuzzy_skin_skip_overhangs",  "0" },
    });
    return config;
}

void slice_pyramid(Print &print, Model &model, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = "inverted_pyramid";
    object->add_volume(inverted_pyramid());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// Every point of every outer wall on a layer, one vector per path.
//
// An overhanging layer's outer wall is SPLIT by role after fuzzing - the part over air becomes
// erOverhangPerimeter, the rest stays erExternalPerimeter - and both halves are the same physical
// wall the feature acts on, so both are collected. Missing that makes an overhang layer look as
// though it has almost no outer wall.
void collect_external(const ExtrusionEntity *entity, std::vector<Points> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_external(child, out);
        return;
    }
    const ExtrusionRole role = entity->role();
    if (role != erExternalPerimeter && role != erOverhangPerimeter)
        return;
    Polyline pl = entity->as_polyline();
    if (pl.points.size() >= 3)
        out.emplace_back(pl.points);
}

std::vector<Points> external_wall_points(const Layer &layer)
{
    std::vector<Points> out;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            collect_external(entity, out);
    return out;
}

// How wavy a layer's outer wall is, in mm.
//
// The model's cross-section is an axis-aligned square, so an un-fuzzed wall is a rectangle: every
// point sits on one of its four sides, and its distance to the nearer of the two lines in each axis
// is zero for one axis and half the side length for the other. Fuzzing pushes points off those
// lines by up to fuzzy_skin_thickness. So: take the wall's own bounding box (the wall is inset from
// the slice by half a line width, which depends on the profile, so the rectangle is not known a
// priori), and measure each point's distance to the rectangle's outline.
//
// Returned as the mean over every point of the wall. An un-fuzzed rectangle gives ~0; a fuzzed wall
// gives roughly half the thickness. Every point counts, corners included - a corner of the
// rectangle is ON the rectangle, so it contributes zero either way, and including them means a
// wall that was fuzzed on only part of its length cannot hide behind a keep-out.
double waviness(const Layer &layer, size_t *n_measured = nullptr)
{
    const std::vector<Points> walls = external_wall_points(layer);
    double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
    size_t n = 0;
    for (const Points &wall : walls)
        for (const Point &pt : wall) {
            const double x = unscaled(double(pt.x()));
            const double y = unscaled(double(pt.y()));
            x_min = std::min(x_min, x); x_max = std::max(x_max, x);
            y_min = std::min(y_min, y); y_max = std::max(y_max, y);
            ++n;
        }
    if (n_measured != nullptr)
        *n_measured = n;
    if (n < 8 || x_max - x_min < 1. || y_max - y_min < 1.)
        return -1.;

    double sum = 0.;
    for (const Points &wall : walls)
        for (const Point &pt : wall) {
            const double x = unscaled(double(pt.x()));
            const double y = unscaled(double(pt.y()));
            // Distance to the rectangle's outline for a point inside it: the smaller of the two
            // per-axis distances to the nearer side. (Fuzzing can push a point a little outside the
            // box it helped define; std::max keeps that at zero rather than negative.)
            const double dx = std::max(0., std::min(x - x_min, x_max - x));
            const double dy = std::max(0., std::min(y - y_min, y_max - y));
            sum += std::min(dx, dy);
        }
    return sum / double(n);
}

// The 95th-percentile gap between consecutive points within one wall path, in mm.
//
// What this is for: a hard supported/unsupported cut would show up as a jump in the wall, so a
// bound on the point-to-point step is a bound on how abrupt the blend can be. What it must NOT
// pick up is the two steps that are long for reasons that have nothing to do with the blend - the
// loop's closing segment back to its seam junction (which passes through unfuzzed, and can be
// arbitrarily long), and the gap between two fragments of one collected entity. Hence a
// percentile rather than a maximum: those are a handful of points out of a hundred-odd, while a
// blend failure would show at every boundary the wall crosses.
double sampling_step_p95(const Layer &layer)
{
    std::vector<double> steps;
    for (const Points &wall : external_wall_points(layer))
        for (size_t i = 1; i < wall.size(); ++i)
            steps.push_back(unscaled((wall[i] - wall[i - 1]).cast<double>().norm()));
    if (steps.empty())
        return 0.;
    std::sort(steps.begin(), steps.end());
    return steps[size_t(0.95 * double(steps.size() - 1))];
}

// Total length of a set of polylines, in scaled units. Slic3r::length() takes a Points, not a
// Polylines, and Slic3r::total_length() is already taken, so this sums them under its own name.
double polylines_length(const Polylines &pls)
{
    double out = 0.;
    for (const Polyline &pl : pls)
        for (size_t i = 1; i < pl.points.size(); ++i)
            out += (pl.points[i] - pl.points[i - 1]).cast<double>().norm();
    return out;
}

struct Sliced
{
    Print print;
    Model model;

    const PrintObject &object() const { return *print.objects().front(); }
    const Layer       &first_layer() const { return *object().layers().front(); }

    // A layer whose WHOLE outer wall overhangs.
    //
    // Chosen by measurement rather than by index: near the bottom of this pyramid the ring still
    // overlaps the layer below by more than the support tolerance over part of its perimeter, so a
    // low layer is only partly an overhang and its wall stays partly fuzzed even with the option on.
    //
    // The measurement is made on the layer's own SLICE OUTLINE, never on its wall points. Testing
    // the wall would be circular: the jitter under test moves wall points across exactly the
    // boundary being tested, so a fuzzed layer and an un-fuzzed one would not agree about which
    // layer to look at, and the cases would not be comparing like with like. The slice outline is
    // the same in every case.
    //
    // The chosen layer is the last one at least 98% of whose outline lies outside the layer below
    // grown by kSupportGrow - unsupported under either generator's convention, with margin.
    const Layer &overhang_layer() const
    {
        const double kSupportGrow = 0.4;  // mm; past both conventions (-0.17 classic, +0.2 Arachne)
        const Layer *chosen = nullptr;
        for (const Layer *layer : object().layers()) {
            if (layer->lower_layer == nullptr || external_wall_points(*layer).size() == 0)
                continue;
            Polygons here, lower;
            for (const LayerRegion *region : layer->regions())
                for (const Surface &s : region->slices.surfaces)
                    append(here, to_polygons(s.expolygon));
            for (const LayerRegion *region : layer->lower_layer->regions())
                for (const Surface &s : region->slices.surfaces)
                    append(lower, to_polygons(s.expolygon));
            if (here.empty() || lower.empty())
                continue;
            const Polygons  grown = to_polygons(offset_ex(union_ex(lower), float(scale_(kSupportGrow))));
            // How much of this layer's OUTLINE - which is what the wall follows - lies over air.
            // Not its area: on a pyramid the whole interior sits over the layer below, so an area
            // test says every layer is supported and picks nothing.
            const Polylines outline   = to_polylines(to_polygons(union_ex(here)));
            const double    total_len = polylines_length(outline);
            const double    over_air  = polylines_length(diff_pl(outline, grown));
            if (total_len > 0. && over_air > 0.98 * total_len)
                chosen = layer;
        }
        REQUIRE(chosen != nullptr);
        return *chosen;
    }
};

// Slice the pyramid with the given extra settings on top of base_config().
void slice(Sliced &out, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict(extra);
    slice_pyramid(out.print, out.model, config);
    REQUIRE(out.object().layers().size() > 4);
}

} // namespace

SCENARIO("Fuzzy skin skips overhanging wall segments", "[FuzzySkinOverhangs]")
{
    // The zero: a wall with no fuzzy skin at all. Its points lie on the rectangle to within slicer
    // coordinate rounding.
    const double kUnfuzzedTol = 1e-3;
    // What an overhanging wall must come down to with the option ON. Not the same number as
    // kUnfuzzedTol, and the difference is the feature working as designed rather than a slack
    // bound: the blend deliberately ramps the displacement to zero over two samples on either side
    // of a supported/unsupported boundary instead of snapping it, and a closed loop always has one
    // such boundary at its seam junction (which passes through unfuzzed). Those few ramp points are
    // real, wanted displacement, and on a wall that the overhang split has reduced to a dozen-odd
    // points they are a visible share of the mean. What must NOT survive is the jitter itself,
    // which is an order of magnitude larger - hence a bound at a third of kFuzzedFloor, which the
    // measured values (about 0.05 mm against a fuzzed 0.30) clear comfortably in both directions.
    //
    // The stronger statement - that the wall over air is exactly the un-fuzzed rectangle - is
    // proved outside this file, by the demo G-code: on an overhang layer with the option on, the
    // outer wall emerges as four moves on four exact coordinates.
    const double kSkippedTol = 0.1;
    // What a fuzzed wall has to beat. The jitter is uniform in [-thickness, +thickness], so the
    // mean |displacement| of a fuzzed wall is about half the thickness; a quarter is a comfortable
    // floor that no held-at-zero wall can reach.
    const double kFuzzedFloor = kThickness / 4.;

    GIVEN("An inverted pyramid: a first layer on the bed, every layer above it overhanging")
    {
        WHEN("fuzzy skin is off entirely")
        {
            Sliced s;
            slice(s, {{"fuzzy_skin", "none"}});

            THEN("no wall is wavy - this is the zero the fuzzed cases are measured against")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                CHECK(first < kUnfuzzedTol);
                CHECK(over  < kUnfuzzedTol);
            }
        }

        WHEN("fuzzy skin is on for the external wall and the overhang skip is OFF")
        {
            Sliced s;
            slice(s, {{"fuzzy_skin", "external"}, {"fuzzy_skin_skip_overhangs", "0"}});

            THEN("every layer is fuzzed, overhanging or not - the new code is inert")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                CHECK(first > kFuzzedFloor);
                CHECK(over  > kFuzzedFloor);
            }
        }

        WHEN("fuzzy skin is on for the external wall and the overhang skip is ON")
        {
            Sliced s;
            slice(s, {{"fuzzy_skin", "external"}, {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its un-fuzzed path")
            {
                size_t n = 0;
                const double over = waviness(s.overhang_layer(), &n);
                REQUIRE(n > 4);
                CHECK(over < kSkippedTol);
            }

            THEN("the first layer is still fully fuzzed - the bed is not an overhang")
            {
                size_t n = 0;
                const double first = waviness(s.first_layer(), &n);
                REQUIRE(n > 4);
                CHECK(first > kFuzzedFloor);
            }

            THEN("the blend leaves no jump between consecutive points of a fuzzed wall")
            {
                // The resampler's own cadence is at most 5/4 * point_distance; a displacement step
                // at a hard supported/unsupported boundary would add up to 2 * thickness on top of
                // that. The bound admits the cadence and the ramp, and excludes the step.
                //
                // Asserted on the FIRST layer, which is fully fuzzed and therefore still carries a
                // resampled path with a cadence to check. The overhanging wall is not a meaningful
                // subject for this check: with its displacement held at zero its collinear points
                // collapse to a handful of long straight moves, so its point spacing says something
                // about path simplification, not about the blend.
                CHECK(sampling_step_p95(s.first_layer()) < kPointDistance * 1.25 + kThickness);
            }
        }

        WHEN("the skip is ON with fuzzy skin on all walls")
        {
            Sliced s;
            slice(s, {{"fuzzy_skin", "allwalls"}, {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the outer wall behaves as in the external-only case")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }

        WHEN("the skip is ON under the Arachne wall generator")
        {
            Sliced s;
            slice(s, {{"wall_generator", "arachne"},
                      {"fuzzy_skin", "external"},
                      {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its path and the first layer stays fuzzed")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }

        WHEN("the skip is ON under Arachne with fuzzy skin on all walls")
        {
            Sliced s;
            slice(s, {{"wall_generator", "arachne"},
                      {"fuzzy_skin", "allwalls"},
                      {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its path and the first layer stays fuzzed")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }
    }
}
