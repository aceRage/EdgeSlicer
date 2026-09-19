// Fuzzy skin over overhangs - the gate for
// docs/superpowers/specs/2026-09-09-fuzzy-skin-overhang-research.md.
//
// The option is fuzzy_skin_skip_overhangs. With it on, the sampled points of a fuzzed wall that
// hang over air keep their un-jittered position, while the points sitting on the layer below are
// jittered as before, with a short linear ramp between the two so there is no step.
//
// Two models, two scenarios.
//
// The inverted pyramid makes the ON/OFF contrast testable without classifying individual points:
// its first layer is a square on the BED, so nothing there is an overhang, while every layer above
// it steps about 0.56 mm past the layer below, so its whole outer wall is an overhang.
//
//  * OFF is the hard gate. With fuzzy_skin_skip_overhangs at its default the new code must not run
//    at all - every layer, overhanging or not, is fuzzed exactly as before. The byte-identity half
//    of that guarantee is the Bar A comparison against a build of this branch's base commit,
//    recorded in the spec; what is asserted here is that no skipping leaks in.
//  * ON: the overhanging layers lose their jitter (their walls become straight to within 1e-3 mm),
//    while the first layer keeps every bit of it - the first layer is bed-supported and is never an
//    overhang.
//  * The fuzzer's own cadence bound on the first layer: consecutive samples of a fuzzed wall are
//    never further apart than the resampler and the jitter allow. The blend is inert on the first
//    layer by design (fuzzy_skip_overhangs_wanted returns false at layer_id <= raft_layers), so this
//    is a property of the plain fuzzer, checked against the exact maximum it has - allowing for the
//    wall simplifier, which cannot be turned off (see base_config) and folds the odd sample.
//
// The pyramid cannot show the blend itself: every sample of an overhanging layer is over air, so the
// factors are 0 all round and there is no boundary to ramp across. The overhang wedge is the model
// for that: one face leans OUT at 70 degrees (an overhang on every layer), the other three lean IN
// at 60 degrees (carried by the layer below with 0.39 mm to spare), so every layer's outer wall has
// exactly one unsupported run with a supported run on either side of it - the configuration the
// ramp exists for. Its assertions are exact: an unsupported sample is emitted at its un-jittered
// position, so it lies ON the wall rectangle to the nanometre, which identifies the run without
// reconstructing the generator's support polygon; and the samples on either side of the run must
// then respect the ramp, read as a bound on displacement against distance along the wall, which
// holds whatever the simplifier folds and which a hard cut trips.
//
// Both fuzzy modes (external only, all walls) and both generators (classic, Arachne) are covered.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// A hexahedron with a rectangular foot on the bed and a rectangular top, every side face planar
// because each of its x and y extents is an affine function of z.
TriangleMesh frustum(float x0_min, float x0_max, float y0_min, float y0_max,
                     float x1_min, float x1_max, float y1_min, float y1_max, float h)
{
    indexed_triangle_set its;
    its.vertices = {
        { x0_min, y0_min, 0.f }, { x0_max, y0_min, 0.f }, { x0_max, y0_max, 0.f }, { x0_min, y0_max, 0.f },
        { x1_min, y1_min, h   }, { x1_max, y1_min, h   }, { x1_max, y1_max, h   }, { x1_min, y1_max, h   },
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

// An inverted pyramid: a 10x10 mm foot on the bed widening to 38x38 mm at 5 mm up, i.e. every side
// about 70 degrees from vertical. Each 0.2 mm layer therefore steps about 0.56 mm past the layer
// below it, which is well clear of the support tolerance the generators use (about -0.17 mm for the
// classic generator's index-0 offset, +0.2 mm for Arachne's), so every wall above the first layer is
// unambiguously an overhang - while the first layer sits flat on the bed and is unambiguously not.
//
// The slope matters. At the 45 degrees one would reach for first, the step per 0.2 mm layer is
// 0.2 mm, SMALLER than the support tolerance itself, so no wall point is ever unambiguously
// unsupported and the test has nothing to measure.
//
// A 10 mm foot, not the 2 mm one the shape suggests: the first layer is the control that proves
// the first-layer rule, and its wall has to be big enough for the four sides of the ring to be
// told apart at all.
TriangleMesh inverted_pyramid()
{
    const float r0 = 5.f, r1 = 19.f;  // half-widths at z = 0 and z = 5 -> the same ~70 degree slope
    return frustum(-r0, r0, -r0, r0, -r1, r1, -r1, r1, 5.f);
}

// The overhang wedge: a 24x24 mm foot, 3 mm high. Its +x face leans OUT at 70 degrees from vertical
// (0.55 mm past the layer below per 0.2 mm layer, an overhang on every layer above the first by
// either generator's convention), its -x, +y and -y faces lean IN at 60 degrees (0.35 mm inside the
// layer below per layer). On every layer the outer wall is therefore an axis-aligned rectangle whose
// +x side hangs over air and whose other three sides are carried by the layer below with margin:
// the classic support polygon is the lower slice shrunk by about 0.17 mm, and an inward-leaning
// side's wall sits 0.35 + 0.21 - 0.17 = 0.39 mm inside it, more than the 0.3 mm the jitter can move
// a point, so the role split cannot fragment a supported side either.
TriangleMesh overhang_wedge()
{
    const float h  = 3.f;
    const float in = h * std::tan(60.f * float(M_PI) / 180.f);  // 5.20 mm
    const float out = h * std::tan(70.f * float(M_PI) / 180.f); // 8.24 mm
    return frustum(-12.f, 12.f, -12.f, 12.f, -12.f + in, 12.f + out, -12.f + in, 12.f - in, h);
}

DynamicPrintConfig base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "layer_height",               "0.2" },
        { "initial_layer_print_height", "0.2" },
        { "wall_loops",                 "2" },
        // The point of the models is their unsupported walls; supports would take that away.
        { "enable_support",             "0" },
        // The overhang split is what supplies the lower-layer polygons the option reuses.
        { "detect_overhang_wall",       "1" },
        // Arc fitting rewrites wall point sets after the fact, and the loop-reversal heuristic
        // reorders them; both would make the geometry below say something other than what it means.
        { "enable_arc_fitting",         "0" },
        { "overhang_reverse",           "0" },
        { "spiral_mode",                "0" },
        // The wall paths are Douglas-Peucker simplified at this tolerance AFTER the perimeter
        // generator is done with them (PrintObject::simplify_extrusion_path). At its 0.01 mm default
        // that drops every fuzzed sample that happens to land within 10 um of the chord between its
        // neighbours - with a +-0.3 mm uniform jitter that is about one sample in thirty, one or two
        // per loop, and each one merges two sampling steps into one. What the assertions below
        // measure is the fuzzer's own sample set, so this asks for the smallest tolerance there is.
        // That is NOT zero: Print::apply runs DynamicPrintConfig::normalize_fdm_1, which clamps the
        // value to 0.001 mm, so the walls under test are simplified at 1 um and about one sample in
        // 300 is still folded. Every assertion below is written to be exact under that - none of
        // them counts samples by index.
        { "resolution",                 "0" },
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

void slice_mesh(Print &print, Model &model, TriangleMesh mesh, const char *name, const DynamicPrintConfig &config)
{
    ModelObject *object = model.add_object();
    object->name = name;
    object->add_volume(std::move(mesh));
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

void collect_loops(const ExtrusionEntity *entity, std::vector<const ExtrusionLoop *> &out)
{
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        for (const ExtrusionEntity *child : coll->entities)
            collect_loops(child, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        out.push_back(loop);
    }
}

// The layer's outer wall: the wall loop with the largest bounding box. Both models are one island
// per layer, so there is exactly one.
//
// Chosen by geometry, NOT by role. An overhanging layer's outer wall is SPLIT by role after fuzzing
// - the part over air becomes erOverhangPerimeter, the rest stays erExternalPerimeter - and so is
// the inner wall on such a layer, into erPerimeter and erOverhangPerimeter. A loop's own role() is
// its first path's, so "role is external or overhang" picks up the inner loop whenever its first
// path happens to be an overhang piece, which depends on where the fuzz put the loop's start; with
// fuzzy_skin = allwalls that inner loop is wavy and sits a full spacing inside, and one run in a
// hundred read a waviness of 0.41 mm on a wall that is straight. The bounding box cannot be fooled.
const ExtrusionLoop *outer_loop(const Layer &layer)
{
    std::vector<const ExtrusionLoop *> loops;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            collect_loops(entity, loops);
    const ExtrusionLoop *outer = nullptr;
    double               best  = -1.;
    for (const ExtrusionLoop *loop : loops) {
        const BoundingBox bb(loop->polygon().points);
        const double      area = double(bb.size().x()) * double(bb.size().y());
        if (area > best) {
            best  = area;
            outer = loop;
        }
    }
    return outer;
}

// Every point of the outer wall, in loop order, closed (the first point repeated at the end).
std::vector<Points> external_wall_points(const Layer &layer)
{
    std::vector<Points>  out;
    const ExtrusionLoop *loop = outer_loop(layer);
    if (loop != nullptr) {
        Polyline pl = loop->as_polyline();
        if (pl.points.size() >= 3)
            out.emplace_back(std::move(pl.points));
    }
    return out;
}

// How wavy a layer's outer wall is, in mm.
//
// The models' cross-sections are axis-aligned rectangles, so an un-fuzzed wall is a rectangle: every
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

// The steps between consecutive points within one wall path, largest first, each with its two
// endpoints in mm so that a failure names the geometry it failed on instead of a bare number.
struct WallStep { double len; Vec2d a, b; size_t wall, idx; };

std::vector<WallStep> wall_steps_descending(const Layer &layer)
{
    std::vector<WallStep> steps;
    size_t            w = 0;
    for (const Points &wall : external_wall_points(layer)) {
        for (size_t i = 1; i < wall.size(); ++i) {
            const Vec2d a = unscale(wall[i - 1]), b = unscale(wall[i]);
            steps.push_back({(b - a).norm(), a, b, w, i});
        }
        ++w;
    }
    std::sort(steps.begin(), steps.end(), [](const WallStep &l, const WallStep &r) { return l.len > r.len; });
    return steps;
}

std::string describe_steps(const std::vector<WallStep> &steps, size_t k = 6)
{
    char        buf[160];
    std::string out = "steps=" + std::to_string(steps.size()) + "\n";
    for (size_t i = 0; i < std::min(k, steps.size()); ++i) {
        const WallStep &s = steps[i];
        std::snprintf(buf, sizeof(buf), "  #%zu wall %zu idx %zu len %.4f (%.3f,%.3f)->(%.3f,%.3f)\n",
                      i, s.wall, s.idx, s.len, s.a.x(), s.a.y(), s.b.x(), s.b.y());
        out += buf;
    }
    return out;
}

// Diagnostic: the layer's perimeter entity tree, one line per entity, with every path's role, point
// count and end points.
void describe_entity(const ExtrusionEntity *entity, int depth, std::string &out)
{
    char buf[200];
    if (const auto *coll = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
        std::snprintf(buf, sizeof(buf), "%*scollection(%zu)\n", depth * 2, "", coll->entities.size());
        out += buf;
        for (const ExtrusionEntity *child : coll->entities)
            describe_entity(child, depth + 1, out);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity)) {
        std::snprintf(buf, sizeof(buf), "%*sloop paths=%zu:", depth * 2, "", loop->paths.size());
        out += buf;
        for (const ExtrusionPath &path : loop->paths) {
            const Vec2d f = unscale(path.polyline.points.front()), l = unscale(path.polyline.points.back());
            std::snprintf(buf, sizeof(buf), " [role %d n %zu (%.3f,%.3f)->(%.3f,%.3f)]", int(path.role()),
                          path.polyline.points.size(), f.x(), f.y(), l.x(), l.y());
            out += buf;
        }
        out += "\n";
    } else {
        std::snprintf(buf, sizeof(buf), "%*sentity role %d n %zu\n", depth * 2, "", int(entity->role()),
                      entity->as_polyline().points.size());
        out += buf;
    }
}

std::string describe_perimeters(const Layer &layer)
{
    std::string out;
    for (const LayerRegion *region : layer.regions())
        for (const ExtrusionEntity *entity : region->perimeters.entities)
            describe_entity(entity, 0, out);
    return out;
}

// Diagnostic for waviness(): the collected outer-wall points' bounding box and the k points
// furthest from that rectangle's outline, plus the entity tree.
std::string describe_waviness(const Layer &layer, size_t k = 5)
{
    struct Far { double d; Vec2d p; size_t wall, idx; };
    std::vector<Far>          outliers;
    const std::vector<Points> walls = external_wall_points(layer);
    double x_min = 1e30, x_max = -1e30, y_min = 1e30, y_max = -1e30;
    for (const Points &wall : walls)
        for (const Point &pt : wall) {
            const Vec2d p = unscale(pt);
            x_min = std::min(x_min, p.x()); x_max = std::max(x_max, p.x());
            y_min = std::min(y_min, p.y()); y_max = std::max(y_max, p.y());
        }
    for (size_t w = 0; w < walls.size(); ++w)
        for (size_t i = 0; i < walls[w].size(); ++i) {
            const Vec2d  p  = unscale(walls[w][i]);
            const double dx = std::max(0., std::min(p.x() - x_min, x_max - p.x()));
            const double dy = std::max(0., std::min(p.y() - y_min, y_max - p.y()));
            outliers.push_back({std::min(dx, dy), p, w, i});
        }
    std::sort(outliers.begin(), outliers.end(), [](const Far &l, const Far &r) { return l.d > r.d; });
    char        buf[200];
    std::snprintf(buf, sizeof(buf), "layer z=%.3f walls=%zu points=%zu bbox x[%.4f %.4f] y[%.4f %.4f]\n",
                  layer.print_z, walls.size(), outliers.size(), x_min, x_max, y_min, y_max);
    std::string out = buf;
    for (size_t i = 0; i < std::min(k, outliers.size()); ++i) {
        std::snprintf(buf, sizeof(buf), "  wall %zu idx %zu d=%.4f (%.4f,%.4f)\n", outliers[i].wall, outliers[i].idx, outliers[i].d, outliers[i].p.x(), outliers[i].p.y());
        out += buf;
    }
    return out + describe_perimeters(layer);
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

// Slice a model with the given extra settings on top of base_config().
void slice(Sliced &out, TriangleMesh mesh, const char *name, std::initializer_list<Slic3r::ConfigBase::SetDeserializeItem> extra)
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict(extra);
    slice_mesh(out.print, out.model, std::move(mesh), name, config);
    REQUIRE(out.object().layers().size() > 4);
}

// ---- the blend on a partly supported wall (the wedge) ----------------------------------------

// The outer wall's SAMPLES, in loop order, as the fuzzer emitted them.
//
// The role split clips the fuzzed loop against the grown lower slices, which inserts a point where
// the loop crosses the clip polygon and starts a new path there. A junction between a wall path and
// an overhang path is therefore a crossing point, not a sample, and is dropped, so that what
// remains is the fuzzer's sample sequence - less the odd sample the 1 um simplifier folds (see
// base_config), which is why check_blend_ramp reads the ramp by distance along the wall and never
// by sample index.
//
// A junction between two paths of the SAME role is something else: the loop's own start vertex,
// which the classic split recombines away (_clipper_pl_recombine) but the Arachne split leaves in
// place. That point IS a sample - fuzzy_extrusion_line moves the loop's un-jittered start vertex
// onto the last sample it emitted - and it is kept, but its index is recorded as the seam: the
// samples on either side of it had their blend factors computed with the start vertex (a
// pass-through counted as supported) and with the samples the closing trim then dropped between
// them, so the ramp cannot be read off the output there. check_blend_ramp skips a run end that the
// seam sits next to.
struct OuterWall
{
    Points samples;
    size_t seam = size_t(-1);  // index into samples of the loop's start vertex, if the split kept it
    bool   has_external = false;
    bool   has_overhang = false;
};

OuterWall outer_wall_samples(const Layer &layer)
{
    const ExtrusionLoop *loop = outer_loop(layer);
    REQUIRE(loop != nullptr);
    OuterWall            out;
    const size_t         n_paths = loop->paths.size();
    const auto           push    = [&out](const Point &p) {
        if (out.samples.empty() || out.samples.back() != p)
            out.samples.push_back(p);
    };
    for (size_t k = 0; k < n_paths; ++k) {
        const ExtrusionPath &path = loop->paths[k];
        const ExtrusionPath &next = loop->paths[(k + 1) % n_paths];
        out.has_external |= path.role() == erExternalPerimeter;
        out.has_overhang |= path.role() == erOverhangPerimeter;
        const Points &pts = path.polyline.points;
        for (size_t i = 1; i + 1 < pts.size(); ++i)
            push(pts[i]);
        if (n_paths > 1 && path.role() == next.role()) {
            REQUIRE(out.seam == size_t(-1));  // one start vertex per loop
            out.seam = out.samples.size();
            push(pts.back());
        }
    }
    return out;
}

// The un-fuzzed outer wall rectangle, in mm, exact to the coordinate grid. The classic outer wall
// is the slice outline inset by half a line width - one Clipper offset with one delta for all four
// sides - so the un-fuzzed wall is the slice's bounding box shrunk by that half width, and the half
// width itself is read off the output: the +x side is unsupported end to end, so its surviving
// samples sit at the un-jittered +x edge exactly (see check_blend_ramp), which puts that edge at
// the largest sample x. Arachne's outer wall lands on the same four edges (measured: 0 nm off).
// Not the inner wall grown by a spacing: that is a second offset with its own rounding, and it
// came out 2 nm off on two sides, which is more than "exact" can absorb.
struct WallRect
{
    double x_min, x_max, y_min, y_max;

    // Distance from the outline for a point at or near it, the same per-axis rule as waviness().
    double distance(const Vec2d &p) const
    {
        const double dx = std::min(std::abs(p.x() - x_min), std::abs(p.x() - x_max));
        const double dy = std::min(std::abs(p.y() - y_min), std::abs(p.y() - y_max));
        return std::min(dx, dy);
    }
};

WallRect outer_wall_rect(const Layer &layer, const OuterWall &wall)
{
    BoundingBox slice;
    for (const LayerRegion *region : layer.regions())
        for (const Surface &s : region->slices.surfaces)
            slice.merge(BoundingBox(s.expolygon.contour.points));
    coord_t x_max = std::numeric_limits<coord_t>::min();
    for (const Point &p : wall.samples)
        x_max = std::max(x_max, p.x());
    const coord_t half_w = slice.max.x() - x_max;
    REQUIRE(half_w > scaled<coord_t>(0.1));
    REQUIRE(half_w < scaled<coord_t>(0.5));
    return { unscaled<double>(slice.min.x() + half_w), unscaled<double>(x_max),
             unscaled<double>(slice.min.y() + half_w), unscaled<double>(slice.max.y() - half_w) };
}

// A point's coordinate along the un-fuzzed wall outline, counter-clockwise from the bottom-right
// corner, in mm. The side is decided the way the fuzz leaves it decidable: a +x-side sample is
// un-jittered and sits on the +x edge exactly; any other sample within the jitter's reach of the +y
// or -y edge belongs to that side (near the +x corners, where this is used, nothing else is within
// reach); the rest is the -x side. The projection onto the side is exact: the fuzz displaces a
// sample along its edge's perpendicular only, so the projection IS the un-jittered sample point.
double outline_u(const Vec2d &p, const WallRect &r, double exact, double reach)
{
    const double W = r.x_max - r.x_min, H = r.y_max - r.y_min;
    if (std::abs(p.x() - r.x_max) <= exact) return p.y() - r.y_min;                 // +x side, upward
    if (std::abs(p.y() - r.y_max) <= reach)  return H + (r.x_max - p.x());          // +y side, leftward
    if (std::abs(p.y() - r.y_min) <= reach)  return H + W + H + (p.x() - r.x_min);  // -y side, rightward
    return H + W + (r.y_max - p.y());                                                // -x side, downward
}

// The claim, on one layer of the wedge with the option ON:
//
//  1. the layer's outer wall is partly supported - the role split found both roles on it, so the
//     blend has a boundary to ramp across (without this the check would be vacuous);
//  2. every sample on the overhanging +x side is emitted exactly un-jittered;
//  3. walking away from the unsupported run in either direction, a supported sample's displacement
//     is bounded by the ramp - factor k/3 for the k-th sample from an unsupported one, kBlendSamples
//     = 2 in FuzzySkin.cpp - read against the sample's distance along the wall, which is the form
//     that survives the simplifier (the derivation is at the check). The ramp cannot exceed it; a
//     hard cut puts full-amplitude samples right next to the run and trips it;
//  4. the supported run away from the ramp is fuzzed as usual.
//
// The run is found from the output: a factor-0 sample sits ON the rectangle to the nanometre, a
// ramped one is off it by |r|/3 with r uniform in +-thickness, so "on the rectangle within 2 nm" is
// the unsupported set. (The one way to mis-identify the run is a ramped sample whose own draw is
// under 6 nm in magnitude, about one draw in 50,000 per boundary, which moves the run's end one
// sample out and tightens the bound on the sample after it by one cadence; that is the residual
// flake rate of this check, about one run in 100,000, and it is stated here so nobody hunts for a
// blend bug over it.)
void check_blend_ramp(const Layer &layer)
{
    const double kExact = 2e-6;  // 2 nm: integer-nanometre coordinates, one rounding either way
    const double kSlack = 1e-5;  // for the ramp bounds: the derived rectangle plus a truncation

    const OuterWall wall = outer_wall_samples(layer);
    INFO("roles: external=" << wall.has_external << " overhang=" << wall.has_overhang
         << " samples=" << wall.samples.size());
    REQUIRE(wall.has_external);
    REQUIRE(wall.has_overhang);
    const size_t n = wall.samples.size();
    REQUIRE(n >= 16);

    const WallRect      rect = outer_wall_rect(layer, wall);
    std::vector<double> disp(n);
    for (size_t i = 0; i < n; ++i)
        disp[i] = rect.distance(unscale(wall.samples[i]));

    // 2: the overhanging +x side is exactly un-jittered, end to end - in two halves.
    //
    // First, what survives of it. A factor-0 sample is emitted on the un-fuzzed edge to the
    // nanometre, which makes the side's samples exactly collinear, and the simplifier (1 um, see
    // base_config) then folds every interior one: what remains of the side is its two end samples, each within
    // one cadence of a corner, both at the edge's x exactly. So at least two samples sit on the +x
    // edge exactly, and they span the side to within a cadence of either corner. With jitter on the
    // side there would be no such sample at all.
    //
    // Second, every sample within the jitter's reach of the edge - clear of the corners by that
    // same reach, so that no sample of the +y or -y side can be mistaken for one - reads a zero
    // displacement. A jittered sample anywhere on the side would survive the simplifier and land
    // exactly here, non-zero.
    const double kCadence = kPointDistance * 5. / 4.;
    size_t n_edge = 0, reach_fuzzed = 0, any = 0;
    double edge_y_lo = 1e30, edge_y_hi = -1e30;
    for (size_t i = 0; i < n; ++i) {
        const Vec2d p = unscale(wall.samples[i]);
        if (std::abs(p.x() - rect.x_max) <= kExact) {
            any = i;
            ++n_edge;
            edge_y_lo = std::min(edge_y_lo, p.y());
            edge_y_hi = std::max(edge_y_hi, p.y());
        }
        if (p.x() > rect.x_max - (kThickness + 0.01) && p.y() > rect.y_min + kThickness + 0.01 &&
            p.y() < rect.y_max - kThickness - 0.01 && disp[i] > kExact)
            ++reach_fuzzed;
    }
    INFO("samples on the +x edge: " << n_edge << " spanning " << edge_y_hi - edge_y_lo << " of a "
         << rect.y_max - rect.y_min << " mm side; jittered samples within reach of it: " << reach_fuzzed);
    REQUIRE(n_edge >= 2);
    CHECK(edge_y_hi - edge_y_lo > (rect.y_max - rect.y_min) - 2. * kCadence);
    CHECK(reach_fuzzed == 0);

    // The unsupported run: from any +x-side sample, extend over the zero-displacement samples.
    size_t a = any, b = any, run = 1;
    while (run < n && disp[(a + n - 1) % n] <= kExact) { a = (a + n - 1) % n; ++run; }
    while (run < n && disp[(b + 1) % n]     <= kExact) { b = (b + 1) % n;     ++run; }
    REQUIRE(run + 6 <= n);

    // 3: the ramp, read as a bound on displacement against distance along the wall.
    //
    // The ramp is defined per SAMPLE - factor k/3 for the k-th supported sample from an unsupported
    // one - but the k-th sample cannot be identified in the output. The wall simplifier runs at a
    // 1 um floor whatever the config says (DynamicPrintConfig::normalize_fdm_1 clamps resolution to
    // 0.001 mm on apply) and folds a sample that lands within 1 um of the chord between its
    // neighbours, about one sample in 300, so an index-based reading is off by one about one run
    // in 200. What is exact AND fold-proof is a sample's position along the un-fuzzed wall
    // (outline_u): a fold changes nothing about the samples that survive, and consecutive samples
    // are at least 3/4 * point_distance apart along the wall, so the k-th sample from the run's
    // last sample is at least k * 0.6 mm along the wall from it - k <= du / 0.6, and its factor is
    // at most min(1, du / 1.8). The ramp can never exceed that. A hard cut puts factor-1 samples
    // 0.6-1.0 mm from the run, where the bound is 0.33-0.56 of the thickness, and trips it in about
    // 85% of runs over the two ends; the cadence's own spread is what keeps that short of certain.
    const double kFloor    = kPointDistance * 3. / 4.;
    const double perimeter = 2. * ((rect.x_max - rect.x_min) + (rect.y_max - rect.y_min));
    const auto   u_of      = [&](size_t i) { return outline_u(unscale(wall.samples[i]), rect, kExact, kThickness + 0.01); };
    const auto   du        = [&](size_t origin, size_t i) {
        const double d = std::fmod(std::abs(u_of(i) - u_of(origin)), perimeter);
        return std::min(d, perimeter - d);
    };
    const auto   bound     = [&](double d) { return kThickness * std::min(1., d / (3. * kFloor)) + kSlack; };

    // An end of the run that the loop's start vertex sits at (Arachne, see outer_wall_samples)
    // cannot be read. The start vertex is a slot in the fuzzer's sequence that advances 0 mm along
    // the wall - exactly what the spacing argument above must not meet - and it sits at a corner of
    // the +x side, i.e. at one end of the run, whichever way the loop is walked: the last sample
    // before it has its factor counted through it. In the output the seam point is either that end
    // sample itself (the walk ended on the +x side) or the sample next to it (the walk ended on the
    // supported side), so an end is unreadable when the seam is the end sample or within three
    // samples outward of it. Not inward: the +x side is folded to its two ends, so "inward" is the
    // other end. On the wedge that is at most one of the two ends, and at least one must be
    // readable or the check would be vacuous.
    bool read_after = true, read_before = true;
    if (wall.seam != size_t(-1))
        for (size_t k = 0; k <= 3; ++k) {
            if ((b + k) % n == wall.seam)     read_after  = false;
            if ((a + n - k) % n == wall.seam) read_before = false;
        }
    std::string ramp;
    for (size_t k = 1; k <= 2; ++k) {
        char         buf[128];
        const size_t i = (b + k) % n, j = (a + n - k) % n;
        std::snprintf(buf, sizeof(buf), "  after+%zu: %.4f at du %.3f (<= %.4f)  before-%zu: %.4f at du %.3f (<= %.4f)",
                      k, disp[i], du(b, i), bound(du(b, i)), k, disp[j], du(a, j), bound(du(a, j)));
        ramp += buf;
    }
    INFO("run of " << run << " unsupported samples (" << n_edge << " on the +x edge)"
         << (read_after ? "" : "; after end at the seam, not read") << (read_before ? "" : "; before end at the seam, not read")
         << ramp);
    REQUIRE((read_after || read_before));
    for (size_t k = 1; k <= 2; ++k) {
        if (read_after) {
            const size_t i = (b + k) % n;
            CHECK(disp[i] <= bound(du(b, i)));
        }
        if (read_before) {
            const size_t j = (a + n - k) % n;
            CHECK(disp[j] <= bound(du(a, j)));
        }
    }

    // 4: three or more samples from the run the factor is 1 and the wall is fuzzed as usual - mean
    // |displacement| about thickness/2, and a quarter is a floor that no held-at-zero wall reaches.
    double sum = 0.;
    size_t m   = 0;
    for (size_t k = 3; k + 3 <= n - run; ++k) {
        sum += disp[(b + k) % n];
        ++m;
    }
    REQUIRE(m >= 8);
    CHECK(sum / double(m) > kThickness / 4.);
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
    // proved on the wedge below, sample by sample.
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
            slice(s, inverted_pyramid(), "inverted_pyramid", {{"fuzzy_skin", "none"}});

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
            slice(s, inverted_pyramid(), "inverted_pyramid", {{"fuzzy_skin", "external"}, {"fuzzy_skin_skip_overhangs", "0"}});

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
            slice(s, inverted_pyramid(), "inverted_pyramid", {{"fuzzy_skin", "external"}, {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its un-fuzzed path")
            {
                size_t n = 0;
                const double over = waviness(s.overhang_layer(), &n);
                REQUIRE(n > 4);
                INFO(describe_waviness(s.overhang_layer()));
                CHECK(over < kSkippedTol);
            }

            THEN("the first layer is still fully fuzzed - the bed is not an overhang")
            {
                size_t n = 0;
                const double first = waviness(s.first_layer(), &n);
                REQUIRE(n > 4);
                CHECK(first > kFuzzedFloor);
            }

            THEN("no two consecutive samples of the first layer's fuzzed wall are further apart than the fuzzer allows")
            {
                // The blend is inert on the first layer by design (there is no layer below to be
                // unsupported by), so this is the plain fuzzer's own bound, and it is an exact
                // maximum, derived from fuzzy_polyline: the samples are one continuous walk along
                // the wall polygon at a cadence in [3/4, 5/4] * point_distance, and every sample is
                // moved by up to thickness along the perpendicular of its own edge.
                //   - two samples on one edge share that perpendicular, so their step is at most
                //     sqrt(cadence^2 + (2 thickness)^2) = 1.17 mm;
                //   - across a 90 degree corner the two perpendiculars are orthogonal: with a and b
                //     the distances from the corner (a + b <= cadence) the step is at most
                //     sqrt((a + t)^2 + (b + t)^2) <= sqrt((cadence + t)^2 + t^2) = 1.33 mm;
                //   - the wrap from the last sample to the first is the same walk: the walk starts
                //     within min_dist/2 = 3/8 * point_distance of the polygon's first vertex and its
                //     last sample lies within one cadence before that vertex, a corner, so
                //     sqrt((cadence + t)^2 + (3/8 d + t)^2) = 1.43 mm, the largest of the three.
                // The bound is the last one.
                //
                // One thing stands between the fuzzer and this measurement: the wall simplifier. It
                // runs at a 1 um floor whatever the config says (DynamicPrintConfig::normalize_fdm_1
                // clamps resolution to 0.001 mm on apply; base_config asks for 0 and gets that), and
                // it folds a sample that lands within 1 um of the chord between its neighbours -
                // with a +-0.3 mm jitter about one sample in 300, one fifty-sample loop in ten. A
                // fold merges two steps into one of up to two cadences, and is invisible in the
                // geometry (1 um), so it is not a jump. Hence two bounds. The eighth-largest step
                // must respect the single-cadence maximum: that tolerates seven folds in one loop
                // and fails at eight, about one run in 10^10 at the measured fold rate. And no step
                // at all may exceed four cadences, which takes four adjacent folds, about one run in
                // 10^8 - while the wrap coming out un-fuzzed, or an edge being skipped, is 10 mm.
                const double kCadence = kPointDistance * 5. / 4.;
                const double kLeadIn  = kPointDistance * 3. / 8.;
                const double kMaxStep = std::sqrt((kCadence + kThickness) * (kCadence + kThickness) +
                                                  (kLeadIn + kThickness) * (kLeadIn + kThickness));
                const std::vector<WallStep> steps = wall_steps_descending(s.first_layer());
                REQUIRE(steps.size() > 16);
                INFO(describe_steps(steps, 9));
                INFO(describe_perimeters(s.first_layer()));
                CHECK(steps[7].len < kMaxStep + 1e-6);
                CHECK(steps.front().len < 4. * kCadence + 2. * kThickness);
            }
        }

        WHEN("the skip is ON with fuzzy skin on all walls")
        {
            Sliced s;
            slice(s, inverted_pyramid(), "inverted_pyramid", {{"fuzzy_skin", "allwalls"}, {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the outer wall behaves as in the external-only case")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                INFO(describe_waviness(s.overhang_layer()));
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }

        WHEN("the skip is ON under the Arachne wall generator")
        {
            Sliced s;
            slice(s, inverted_pyramid(), "inverted_pyramid",
                  {{"wall_generator", "arachne"},
                   {"fuzzy_skin", "external"},
                   {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its path and the first layer stays fuzzed")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                INFO(describe_waviness(s.overhang_layer()));
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }

        WHEN("the skip is ON under Arachne with fuzzy skin on all walls")
        {
            Sliced s;
            slice(s, inverted_pyramid(), "inverted_pyramid",
                  {{"wall_generator", "arachne"},
                   {"fuzzy_skin", "allwalls"},
                   {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging wall keeps its path and the first layer stays fuzzed")
            {
                size_t n_first = 0, n_over = 0;
                const double first = waviness(s.first_layer(), &n_first);
                const double over  = waviness(s.overhang_layer(), &n_over);
                REQUIRE(n_first > 4);
                REQUIRE(n_over > 4);
                INFO(describe_waviness(s.overhang_layer()));
                CHECK(over  < kSkippedTol);
                CHECK(first > kFuzzedFloor);
            }
        }
    }
}

SCENARIO("Fuzzy skin ramps its displacement where a wall goes from supported to overhanging", "[FuzzySkinOverhangs]")
{
    GIVEN("The overhang wedge: one face leaning out over air, three leaning in onto the layer below")
    {
        // A mid layer: partly supported like every layer above the first, chosen by index because
        // every one of them is the same case. Layer 5 is 1.0-1.2 mm up, with 2 mm of wedge above.
        const auto layer_under_test = [](const Sliced &s) -> const Layer & {
            REQUIRE(s.object().layers().size() > 8);
            const Layer &layer = *s.object().layers()[5];
            REQUIRE(layer.lower_layer != nullptr);
            return layer;
        };

        WHEN("the skip is ON for the external wall under the classic generator")
        {
            Sliced s;
            slice(s, overhang_wedge(), "overhang_wedge", {{"fuzzy_skin", "external"}, {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the overhanging side is exact, the supported sides are fuzzed, and the ramp between them holds")
            {
                check_blend_ramp(layer_under_test(s));
            }
        }

        WHEN("the skip is ON for the external wall under Arachne")
        {
            Sliced s;
            slice(s, overhang_wedge(), "overhang_wedge",
                  {{"wall_generator", "arachne"},
                   {"fuzzy_skin", "external"},
                   {"fuzzy_skin_skip_overhangs", "1"}});

            THEN("the same holds")
            {
                check_blend_ramp(layer_under_test(s));
            }
        }
    }
}
