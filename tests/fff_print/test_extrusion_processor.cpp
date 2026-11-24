// Overhang slowdown estimator tests ported from upstream OrcaSlicer's tests/fff_print/test_extrusion_processor.cpp
// (Catch2 v3 there, v2 here): the cases of Orca #14735 (caged external overhangs, interior sampling). Our
// ExtendedPoint is the 2D form (upstream templated it on the dimension for the belt printer); the Orca #15945 and
// #16050 cases of that file are not here (ours are in test_over_support_surfaces.cpp).

#include <catch2/catch.hpp>

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/ExtrusionProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h> // _msize on Windows, malloc_usable_size on Linux
#endif

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// Print settings the assertions below are derived from.
constexpr double caged_layer_height     = 0.2;  // mm
constexpr double caged_wall_width       = 0.42; // mm, outer wall line width
constexpr double caged_outer_wall_speed = 200.; // mm/s
constexpr double caged_slow_speed       = 100.; // mm/s, between every configured overhang speed (<= 50) and the wall speed

// A wall running 0.2mm out over a previous layer whose edge dishes 0.03mm away from it in the middle,
// standing in for the endpoint readings a caged overhang perimeter takes: enough of a difference to
// print at another speed, but only a fraction of the distance at which slowdown begins.
constexpr double dished_wall_gap     = 0.2;   // mm, how far the wall runs out past the previous layer's edge
constexpr double dished_layer_depth  = 0.03;  // mm, how much further out the middle of it reads
constexpr double dished_min_distance = 0.042; // mm, the reading at which the configured speeds begin to slow down
// Every reading here is past that, so the whole wall is slowed and only the amount is in question.
constexpr float  dished_end_reading  = float(dished_wall_gap + 0.5 * caged_wall_width);
constexpr float  dished_mid_reading  = float(dished_end_reading + dished_layer_depth);
// The two readings are dished_layer_depth apart, so half of that tells them apart while still allowing
// for the points the passes after sampling add, which read a little further out than the ends do.
constexpr double dished_reading_tolerance = 0.5 * dished_layer_depth;

// A 40 x 20 x 20 mm box with a 45 degree overhang cut into the y = 0 side. The sloped face spans
// x = 5.086 .. 34.914 only, so the full-height walls of the box cage both ends of every overhang
// perimeter: the endpoints look supported even though the span between them is not.
TriangleMesh caged_overhang_mesh()
{
    return TriangleMesh(
        {
            {5.0859987f, 10.167065f, 5.711731f}, {34.914257f, 10.167065f, 5.711731f},
            {34.914257f, 0.f, 15.878796f},       {5.0859995f, 0.f, 15.878796f},
            {0.f, 0.f, 0.f},                      {0.f, 0.f, 20.f},
            {0.f, 20.f, 20.f},                    {0.f, 20.f, 0.f},
            {40.f, 20.f, 20.f},                   {40.f, 20.f, 0.f},
            {40.f, 0.f, 20.f},                    {40.f, 0.f, 0.f},
            {34.914257f, 0.f, 0.f},               {5.0859995f, 0.f, 0.f},
            {34.914257f, 10.167065f, 0.f},        {5.0859995f, 10.167065f, 0.f},
        },
        {
            {0, 1, 2},   {0, 2, 3},   {4, 5, 6},   {4, 6, 7},   {7, 6, 8},   {7, 8, 9},
            {9, 8, 10},  {9, 10, 11}, {12, 11, 10}, {5, 4, 13}, {5, 13, 3},  {2, 12, 10},
            {5, 3, 2},   {10, 5, 2},  {9, 11, 12}, {9, 12, 14}, {13, 4, 7},  {9, 14, 15},
            {15, 13, 7}, {7, 9, 15},  {8, 6, 5},   {8, 5, 10},  {14, 1, 0},  {14, 0, 15},
            {2, 1, 14},  {2, 14, 12}, {15, 0, 3},  {15, 3, 13},
        });
}

// Mesh geometry the wall filters below are derived from.
constexpr double caged_box_depth      = 20.;       // mm, the box spans y = 0 .. 20
constexpr double caged_slope_face_sum = 15.878796; // mm, y + z of the sloped face, from its corners
// The sloped face spans this x range; outside it the box walls run full height.
constexpr double caged_slope_x_min = 5.0859995;
constexpr double caged_slope_x_max = 34.914257;
constexpr double caged_slope_span  = caged_slope_x_max - caged_slope_x_min; // ~29.8 mm
// The z range the sloped face occupies, from the same fixture vertices.
constexpr double caged_slope_z_min = 5.711731;
constexpr double caged_slope_z_max = 15.878796;
// The lowest slope layer still sits on the solid body below the notch, so it is fully supported and
// runs at the outer wall speed by design. The caged span proper begins one layer above it.
constexpr double caged_span_z_min = caged_slope_z_min + caged_layer_height;

// A layer printed at z is sliced at z - layer_height / 2, and the outer wall centreline sits half a
// line width inside the contour, so the wall on the slope satisfies y + z = 16.189.
constexpr double caged_slope_wall_sum = caged_slope_face_sum + 0.5 * caged_layer_height + 0.5 * caged_wall_width;
// Same inset on the fully supported y = 20 face, vertical over the whole height.
constexpr double caged_back_wall_y = caged_box_depth - 0.5 * caged_wall_width;
// And on the y = 0 face, which runs full height only outside the slope's x range.
constexpr double caged_front_wall_y = 0.5 * caged_wall_width;
// Arachne varies the wall width along a face, and the centreline inset is half that width, so a
// wall sits within about half a line width of where the nominal inset alone would put it. The
// faces being selected are millimetres apart, so this stays far from ambiguous.
constexpr double caged_wall_tolerance = 0.5 * caged_wall_width;

// Feed rates in mm/min of the long outer wall extrusions `keep_line` selects.
template<typename KeepLine> std::vector<double> outer_wall_feed_rates(const std::string& gcode, KeepLine keep_line)
{
    std::vector<double> feed_rates;
    bool outer_wall = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&feed_rates, &outer_wall, &keep_line](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            outer_wall = comment.find("Outer wall") != std::string_view::npos ||
                         comment.find("External perimeter") != std::string_view::npos;

        if (outer_wall && line.extruding(self) && line.dist_XY(self) > 1.0 && keep_line(self, line))
            feed_rates.push_back(line.new_F(self));
    });

    return feed_rates;
}

// The caged 45 degree overhang: outer walls crossing the sloped face for most of its width, on the
// layers where the face genuinely overhangs.
// Both ends are tested against the slope plane rather than requiring a constant Y. Arachne's
// variable-width walls drift slightly in Y along the same slope (Y6.186 -> Y6.189 on one move), so
// a constant-Y filter matches almost nothing under Arachne and silently reduces its coverage.
// The length test excludes the cage walls: they are only as wide as the box is either side of the
// slope, but being vertical their y + z sweeps through the slope plane as z rises, so a couple of
// their fully supported moves would otherwise be counted as part of the span.
std::vector<double> caged_slope_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const double z = line.new_Z(self);
        return z > caged_span_z_min && z < caged_slope_z_max &&
               line.dist_XY(self) > 0.5 * caged_slope_span &&
               std::abs(self.y() + z - caged_slope_wall_sum) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) + z - caged_slope_wall_sum) < caged_wall_tolerance;
    });
}

// The opposite, fully supported face, skipping the initial layer and its own speed settings.
std::vector<double> back_wall_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return line.new_Z(self) > 1.5 * caged_layer_height &&
               std::abs(self.y() - caged_back_wall_y) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) - caged_back_wall_y) < caged_wall_tolerance;
    });
}

// The first layer printed entirely above the slope. Its y = 0 wall runs the full width of the box.
const double caged_layer_above_slope_z = std::ceil(caged_slope_z_max / caged_layer_height) * caged_layer_height;

// The parts of that wall standing on the cage rather than the slope, so on a contour identical to their own.
// Where the support changes is found by bisection, which stops at spans of 2mm, so the move spanning each end of
// the slope reaches a little way into the cage. Taking only the moves lying wholly outside the slope's x range
// leaves the wall that is unambiguously supported, without asserting how closely the bisection converged.
std::vector<double> cage_shoulder_feed_rates(const std::string& gcode)
{
    return outer_wall_feed_rates(gcode, [](const GCodeReader& self, const GCodeReader::GCodeLine& line) {
        return std::abs(line.new_Z(self) - caged_layer_above_slope_z) < 0.5 * caged_layer_height &&
               std::abs(self.y() - caged_front_wall_y) < caged_wall_tolerance &&
               std::abs(line.new_Y(self) - caged_front_wall_y) < caged_wall_tolerance &&
               (std::max(self.x(), line.new_X(self)) <= caged_slope_x_min ||
                std::min(self.x(), line.new_X(self)) >= caged_slope_x_max);
    });
}

// The readings a 40mm wall takes over a previous layer whose edge falls away by 0.03mm towards the
// middle: both ends read the same, and the middle reads slightly further out over air. Whether that
// middle reading survives is what decides the speed the wall is printed at.
std::vector<ExtendedPoint> sampled_wall_over_dished_layer(const std::function<float(float)>& distance_to_speed)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {20., -dished_layer_depth}},
        {{20., -dished_layer_depth}, {40., 0.}},
        {{40., 0.}, {40., -10.}},
        {{40., -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const Points wall{Point::new_scale(0., dished_wall_gap), Point::new_scale(40., dished_wall_gap)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                              dished_min_distance, distance_to_speed);
}

// A straight, otherwise supported wall over a previous-layer boundary with a 2mm-wide pocket. Moving the
// pocket between x = 10 and x = 20 covers both discovery away from the wall's midpoint and refinement around
// a midpoint that has already been discovered. The current wall is inset half its width from the flat boundary,
// so its supported readings are zero after the estimator applies its boundary offset.
constexpr double narrow_pocket_wall_length = 40.;
constexpr double narrow_pocket_width       = 2.;
constexpr double narrow_pocket_depth       = 0.3;

std::vector<ExtendedPoint> sampled_wall_over_narrow_pocket(
    double pocket_center, const std::function<float(float)>& distance_to_speed)
{
    const double pocket_left  = pocket_center - 0.5 * narrow_pocket_width;
    const double pocket_right = pocket_center + 0.5 * narrow_pocket_width;
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {pocket_left, 0.}},
        {{pocket_left, 0.}, {pocket_left, -narrow_pocket_depth}},
        {{pocket_left, -narrow_pocket_depth}, {pocket_right, -narrow_pocket_depth}},
        {{pocket_right, -narrow_pocket_depth}, {pocket_right, 0.}},
        {{pocket_right, 0.}, {narrow_pocket_wall_length, 0.}},
        {{narrow_pocket_wall_length, 0.}, {narrow_pocket_wall_length, -10.}},
        {{narrow_pocket_wall_length, -10.}, {0., -10.}},
        {{0., -10.}, {0., 0.}},
    });
    const double wall_y = -0.5 * caged_wall_width;
    const Points wall{Point::new_scale(0., wall_y), Point::new_scale(narrow_pocket_wall_length, wall_y)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                               dished_min_distance, distance_to_speed);
}

// A cross section that grows a layer's worth on the two faces meeting at either end of a wall, as any
// 45 degree overhang does. The wall itself stands on a contour identical to its own, but its ends sit
// where the growing faces cut the corners off, and the previous layer's edge there is nearer than the
// half line width the centreline is inset by. Both ends therefore read an overhang while everything
// between them reads supported: the reverse of the caged span, and the case the sampling above must
// leave to the passes after it.
constexpr double stepped_wall_inset = 0.5 * caged_wall_width;                       // mm, centreline inset from the contour
constexpr double stepped_end_gap    = stepped_wall_inset - caged_layer_height;      // mm, how far inside the corner ends up
constexpr double stepped_wall_span  = 30.;                                          // mm, the length of the wall

std::vector<ExtendedPoint> sampled_wall_between_growing_corners(const std::function<float(float)>& distance_to_speed)
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer(std::vector<Linef>{
        {{0., 0.}, {32., 0.}},
        {{32., 0.}, {32., -stepped_wall_span}},
        {{32., -stepped_wall_span}, {0., -stepped_wall_span}},
        {{0., -stepped_wall_span}, {0., 0.}},
    });
    const Points wall{Point::new_scale(stepped_wall_inset, -stepped_end_gap),
                      Point::new_scale(stepped_wall_inset, stepped_end_gap - stepped_wall_span)};

    return estimate_points_properties<true, true, true, true>(wall, prev_layer, caged_wall_width, -1.f,
                                                              dished_min_distance, distance_to_speed);
}

// How much of a path is printed below the speed a fully supported reading gives. A segment is printed
// at the lower of the speeds its ends read.
double slowed_length(const std::vector<ExtendedPoint>& points, const std::function<float(float)>& distance_to_speed)
{
    double length = 0.;
    for (size_t i = 0; i + 1 < points.size(); ++i)
        if (std::min(distance_to_speed(points[i].distance), distance_to_speed(points[i + 1].distance)) < distance_to_speed(0.f))
            length += (points[i + 1].position - points[i].position).norm();
    return length;
}

float furthest_reading(const std::vector<ExtendedPoint>& points)
{
    return std::max_element(points.begin(), points.end(), [](const ExtendedPoint& l, const ExtendedPoint& r) {
               return l.distance < r.distance;
           })->distance;
}

// The overhang settings of these tests, on top of `config`, which gives the printer and its filaments. They set one
// 0.4mm nozzle: a test on more nozzles sets nozzle_diameter again.
DynamicPrintConfig caged_overhang_config(const char* wall_generator,
                                         DynamicPrintConfig config = DynamicPrintConfig::full_print_config()){
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4"},
        {"initial_layer_print_height", caged_layer_height},
        {"layer_height", caged_layer_height},
        {"line_width", caged_wall_width},
        {"outer_wall_line_width", caged_wall_width},
        {"inner_wall_line_width", "0.45"},
        {"wall_loops", "2"},
        {"wall_generator", wall_generator},
        {"wall_sequence", "inner wall/outer wall"},
        {"sparse_infill_density", "15%"},
        {"detect_overhang_wall", "1"},
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "0"},
        {"zaa_enabled", "0"},
        {"outer_wall_speed", caged_outer_wall_speed},
        {"inner_wall_speed", "300"},
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "50"},
        {"overhang_3_4_speed", "30"},
        {"overhang_4_4_speed", "10"},
        {"bridge_speed", "50"},
        {"filament_max_volumetric_speed", "22"},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"}, // Nothing but the overhang settings may lower a wall speed
    });
    return config;
}

std::string caged_overhang_gcode(const char* wall_generator)
{
    // Not arranged: the filters below select walls by their G-code coordinates, which are the mesh's own
    // (upstream calls its init_print with arrange = false; ours always arranges, so the object is set up here).
    Print        print;
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "caged_overhang.stl";
    object->add_volume(caged_overhang_mesh());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(caged_overhang_config(wall_generator));
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return gcode(print);
}

// Reports the matched move count alongside the extremes, so a filter that selected nothing is
// distinguishable from a span that simply was not slowed.
void info_feed_rates(const char* span, const std::vector<double>& feed_rates)
{
    UNSCOPED_INFO("matched " << feed_rates.size() << " " << span << " moves");
    if (!feed_rates.empty()) {
        const auto extremes = std::minmax_element(feed_rates.begin(), feed_rates.end());
        UNSCOPED_INFO("slowest " << *extremes.first / MM_PER_MIN << " mm/s, fastest " << *extremes.second / MM_PER_MIN << " mm/s");
    }
}

} // namespace

// Classic reproduces the endpoint-sampling bug: it emits the span as one long move whose endpoints
// both read as supported, so endpoint-only sampling never slows it. Arachne's endpoints already read
// as overhanging, but their placement near the cage makes the inferred support vary by layer. Arachne
// parity is therefore part of this regression's scope: both generators must classify the unsupported
// interior of the same 45-degree span consistently.
TEST_CASE("Caged external overhangs are slowed along their span", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = caged_slope_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("caged slope", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    // The endpoint bug left Classic at the full wall speed, while Arachne's cage-adjacent endpoint
    // samples selected much faster bands on some layers. The whole span must stay in the slowed range
    // for both generators, without requiring their different path segmentations to match.
    const double fastest = *std::max_element(feed_rates.begin(), feed_rates.end());
    REQUIRE(fastest < caged_slow_speed * MM_PER_MIN);
}

// The other side of the fix: the midpoint probe fires on every long external perimeter, so a
// regression that over-slows would leave the test above green. A fully supported wall must keep the
// speed it was configured with.
TEST_CASE("Supported vertical walls keep their normal speed", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = back_wall_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("back wall", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    const double slowest = *std::min_element(feed_rates.begin(), feed_rates.end());
    REQUIRE(slowest >= caged_slow_speed * MM_PER_MIN);
}

// The slope's top edge falls mid layer, so the first layer above it still stands 0.179mm proud of the layer
// below wherever that layer was still on the slope. That is a real overhang and is slowed, but it ends with the
// slope: outside the slope's x range the box runs full height, so the same wall stands on a contour identical to
// its own. Sampling the interior of that wall at a single point reported one support reading for all of it and
// slowed these fully supported ends along with the rest.
TEST_CASE("Wall sections beside a caged overhang keep their normal speed", "[ExtrusionProcessor][Regression]")
{
    const char* wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    const std::vector<double> feed_rates = cage_shoulder_feed_rates(caged_overhang_gcode(wall_generator));
    info_feed_rates("cage shoulder", feed_rates);

    REQUIRE_FALSE(feed_rates.empty());

    const double slowest = *std::min_element(feed_rates.begin(), feed_rates.end());
    REQUIRE_THAT(slowest / MM_PER_MIN, Catch::Matchers::WithinRel(caged_outer_wall_speed, 0.01));
}

// A wall is printed at the lower of the speeds its ends read, so a reading only earns a point in the
// path where it prints at a different speed from the readings around it. Judging that on the readings
// themselves rather than the speeds they produce was too coarse: the configured speeds interpolate
// between their sections, so readings a fraction of the slowdown threshold apart still print more than
// 10% apart, and a real 45 degree overhang had its true reading dropped as if it agreed with its ends.
// The ends then chose the speed on their own, and being next to the walls either side of the overhang
// they read differently from layer to layer, banding an overhang that should have been uniform.
TEST_CASE("An overhang reading is kept whenever it changes the speed", "[ExtrusionProcessor][Regression]")
{
    // A steep speed curve, of the kind the configured overhang speeds interpolate across.
    const std::vector<ExtendedPoint> points =
        sampled_wall_over_dished_layer([](float distance) { return std::round(200.f - 400.f * distance); });

    REQUIRE_THAT(furthest_reading(points), Catch::Matchers::WithinAbs(dished_mid_reading, dished_reading_tolerance));
}

// The complement, and why the readings alone were tempting: a reading that prints at the same speed as
// its neighbours cannot change the G-code, so sampling must leave the path alone however far out it is.
TEST_CASE("An overhang reading is dropped when the speed is unchanged", "[ExtrusionProcessor]")
{
    // A flat speed curve, of the kind a single configured overhang speed produces.
    const std::vector<ExtendedPoint> points = sampled_wall_over_dished_layer([](float) { return 50.f; });

    REQUIRE_THAT(furthest_reading(points), Catch::Matchers::WithinAbs(dished_end_reading, dished_reading_tolerance));
}

TEST_CASE("Coarse probing detects an unsupported pocket away from the wall midpoint",
          "[ExtrusionProcessor][Regression]")
{
    const std::function<float(float)> distance_to_speed = [](float distance) { return distance <= 0.2f ? 100.f : 50.f; };
    const std::vector<ExtendedPoint> points =
        sampled_wall_over_narrow_pocket(0.25 * narrow_pocket_wall_length, distance_to_speed);
    const double slowed = slowed_length(points, distance_to_speed);

    REQUIRE(slowed > 0.);
    REQUIRE(slowed < 5.);
}

TEST_CASE("Coarse probing brackets a narrow slowdown at the wall midpoint",
          "[ExtrusionProcessor][Regression]")
{
    // Half of the pocket reading still maps to full speed. A matching probe in either half therefore must not
    // prune that half before a supported point has been found close enough to bracket the slow midpoint.
    const std::function<float(float)> distance_to_speed = [](float distance) { return distance <= 0.2f ? 100.f : 50.f; };
    const std::vector<ExtendedPoint> points =
        sampled_wall_over_narrow_pocket(0.5 * narrow_pocket_wall_length, distance_to_speed);
    const double slowed = slowed_length(points, distance_to_speed);

    REQUIRE(slowed > 0.);
    REQUIRE(slowed < 5.);
}

// Sampling probes the interior, so it must not answer for the ends. On a supported wall between two
// corners that read an overhang, the reading that differs is the end's own, and the pass that ends a
// slowdown an end reads places its point from how far out that end is. Sampling took the difference as
// its own to report and put a point at the nearest position bisection had reached instead, which both
// sits further along the wall and leaves too little of it for that pass to run on, so the corner
// slowdown ran millimetres up an otherwise supported wall. Its length grows with the wall, so on a
// model whose cross section keeps growing it reads as a stair stepped band up the corner.
TEST_CASE("A supported wall between overhanging corners is slowed no further than its ends require",
          "[ExtrusionProcessor][Regression]")
{
    // A steep speed curve, so the ends and the interior between them print at clearly different speeds.
    const std::function<float(float)> distance_to_speed = [](float distance) {
        return std::round(float(caged_outer_wall_speed) - 400.f * distance);
    };

    const double sampled   = slowed_length(sampled_wall_between_growing_corners(distance_to_speed), distance_to_speed);
    // The same wall with sampling switched off: what the endpoint driven passes alone make of the corners.
    const double unsampled = slowed_length(sampled_wall_between_growing_corners({}), distance_to_speed);

    // The corners do read an overhang, so there is a slowdown for sampling to have lengthened.
    REQUIRE(unsampled > 0.);
    REQUIRE(sampled <= unsampled);
}

// Edge: the interior sampling above is for walls. A bridge infill line crosses open air between its anchors by
// design and is printed at the bridge speed; probing its interior read that span as an overhang and, with
// "slow down for curled perimeters" on (the last speed section is then the 4/4 overhang speed), printed nearly
// all of it at 10 mm/s. Its interior must be left alone, as before the interior pass existed.
TEST_CASE("Bridge infill is not slowed to the overhang speeds along its span", "[ExtrusionProcessor][Regression]")
{
    const bool slowdown_curled = GENERATE(true, false);
    INFO("slow down for curled perimeters: " << slowdown_curled);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"detect_overhang_wall", "1"},
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", slowdown_curled ? "1" : "0"},
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "50"},
        {"overhang_3_4_speed", "30"},
        {"overhang_4_4_speed", "10"},
        {"bridge_speed", "50"},
        {"filament_max_volumetric_speed", "40"},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
        {"enable_support", "0"},
    });
    // TestMesh::bridge: a 50x10mm deck on two 5mm-wide pillars, a 40mm span.
    const std::string gcode_out = Slic3r::Test::slice({TestMesh::bridge}, config);

    double bridge_length = 0., slow_length = 0.;
    bool   bridge_infill = false;
    GCodeReader parser;
    parser.parse_buffer(gcode_out, [&](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            bridge_infill = comment.find("Bridge") != std::string_view::npos && comment.find("Internal") == std::string_view::npos;
        if (bridge_infill && line.extruding(self) && line.dist_XY(self) > 0.) {
            bridge_length += line.dist_XY(self);
            if (line.new_F(self) <= 10. * MM_PER_MIN + EPSILON)
                slow_length += line.dist_XY(self);
        }
    });
    UNSCOPED_INFO("bridge infill " << bridge_length << " mm, at the 4/4 overhang speed " << slow_length << " mm");

    REQUIRE(bridge_length > 100.);
    CHECK(slow_length < 0.05 * bridge_length);
}

// Orca #17144: speeds in mm/s of the bridge infill moves over the middle half of TestMesh::bridge's first deck layer.
static std::vector<double> bridge_center_feed_rates(const std::string& gcode)
{
    struct BridgeMove { double start_x, end_x, speed; };
    std::vector<BridgeMove> bridge_moves;
    bool bridge = false;
    GCodeReader parser;
    parser.parse_buffer(gcode, [&bridge_moves, &bridge](GCodeReader& self, const GCodeReader::GCodeLine& line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            bridge = comment.find("Bridge") != std::string_view::npos && comment.find("Internal Bridge") == std::string_view::npos;

        const double start_x = self.x();
        const double end_x   = line.new_X(self);
        if (bridge && line.extruding(self) && std::abs(line.new_Z(self) - 5.2) < 0.01 &&
            std::abs(end_x - start_x) > 1.0)
            bridge_moves.push_back({start_x, end_x, line.new_F(self) / MM_PER_MIN});
    });

    if (bridge_moves.empty())
        return {};

    double min_x = std::numeric_limits<double>::max();
    double max_x = std::numeric_limits<double>::lowest();
    for (const BridgeMove& move : bridge_moves) {
        min_x = std::min({min_x, move.start_x, move.end_x});
        max_x = std::max({max_x, move.start_x, move.end_x});
    }

    const double center_margin = 0.25 * (max_x - min_x);
    std::vector<double> feed_rates;
    for (const BridgeMove& move : bridge_moves) {
        const double mid_x = 0.5 * (move.start_x + move.end_x);
        if (mid_x > min_x + center_margin && mid_x < max_x - center_margin)
            feed_rates.push_back(move.speed);
    }
    return feed_rates;
}

// Orca #17144: with the curled perimeter slowdown on, the fully unsupported entry of the speed table was the
// 4/4 overhang speed for every role, so an unsupported point of a bridge strand got 10 mm/s.
TEST_CASE("Bridge infill keeps bridge speed with curled perimeter slowdown", "[ExtrusionProcessor][Regression]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"initial_layer_print_height", "0.2"},
        {"layer_height", "0.2"},
        {"enable_overhang_speed", "1"},
        {"slowdown_for_curled_perimeters", "1"},
        {"overhang_1_4_speed", "0"},
        {"overhang_2_4_speed", "50"},
        {"overhang_3_4_speed", "30"},
        {"overhang_4_4_speed", "10"},
        {"bridge_speed", "45"},
        {"filament_max_volumetric_speed", "1000"},
        {"slow_down_for_layer_cooling", "0"},
        {"slow_down_layers", "0"},
    });

    const std::vector<double> feed_rates = bridge_center_feed_rates(Slic3r::Test::slice({TestMesh::bridge}, config));

    REQUIRE_FALSE(feed_rates.empty());
    for (const double feed_rate : feed_rates)
        CHECK_THAT(feed_rate, Catch::Matchers::WithinAbs(45.0, 0.5));
}

// ---------------------------------------------------------------------------------------------------------------
// Errors the Rust port of the overhang passes turned up.

namespace {

// A comb's outline as previous-layer lines, clockwise like the layers above (the inside lies to the right of each
// line): a bar from x = 1 to 11 and y = 1 to 5, with three teeth hanging down to y = -1 at x = 1..3, 5..7 and 9..11.
// The line y = 0 from x = 0 to x = 10 crosses it five times, at x = 1, 3, 5, 7 and 9, and ends inside a tooth.
AABBTreeLines::LinesDistancer<Linef> comb_outline()
{
    const std::vector<Vec2d> ccw{{1., -1.}, {3., -1.}, {3., 1.}, {5., 1.}, {5., -1.}, {7., -1.},
                                 {7., 1.},  {9., 1.},  {9., -1.}, {11., -1.}, {11., 5.}, {1., 5.}};
    std::vector<Linef> lines;
    for (size_t i = ccw.size(); i > 0; --i)
        lines.emplace_back(ccw[i % ccw.size()], ccw[i - 1]);
    return AABBTreeLines::LinesDistancer<Linef>(std::move(lines));
}

// Edge: a scope in which every block the test thread frees is overwritten with 0xFF bytes (a double reads as NaN)
// and held back from the allocator until the scope ends. A read of freed memory then sees the poison every time,
// on every platform, instead of whatever the allocator happened to leave there (usually the old bytes, which is
// why the case below passed before the fix without the address sanitiser). Only this thread, only inside the scope;
// everywhere else operator delete is the plain free() the default one is.
thread_local bool   edge_poison_freed_memory = false;
thread_local void  *edge_quarantine[256];
thread_local size_t edge_quarantined = 0;

void edge_poison_release(void *ptr, std::size_t size) noexcept
{
    if (edge_quarantined < sizeof(edge_quarantine) / sizeof(edge_quarantine[0])) {
        std::memset(ptr, 0xFF, size);
        edge_quarantine[edge_quarantined++] = ptr;
    } else
        std::free(ptr);
}

struct PoisonFreedMemory
{
    PoisonFreedMemory() { edge_poison_freed_memory = true; }
    ~PoisonFreedMemory()
    {
        edge_poison_freed_memory = false;
        for (size_t i = 0; i < edge_quarantined; ++i)
            std::free(edge_quarantine[i]);
        edge_quarantined = 0;
    }
};

} // namespace

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EDGE_TEST_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define EDGE_TEST_ASAN 1
#endif

#ifndef EDGE_TEST_ASAN
// Replaces the default operator delete of this test program (the default operator new allocates with malloc on
// MSVC, libc++ and libstdc++, so free() is its counterpart). Left out under the address sanitiser, which replaces
// these itself and reports the read on its own.
void operator delete(void *ptr) noexcept
{
    if (ptr == nullptr)
        return;
    if (!edge_poison_freed_memory) {
        std::free(ptr);
        return;
    }
#if defined(_WIN32)
    edge_poison_release(ptr, _msize(ptr));
#elif defined(__APPLE__)
    edge_poison_release(ptr, malloc_size(ptr));
#else
    edge_poison_release(ptr, malloc_usable_size(ptr));
#endif
}

void operator delete(void *ptr, std::size_t size) noexcept
{
    if (ptr == nullptr)
        return;
    if (!edge_poison_freed_memory) {
        std::free(ptr);
        return;
    }
    edge_poison_release(ptr, size);
}
#endif // EDGE_TEST_ASAN

// estimate_points_properties held a reference to the last point of the vector it was filling while it pushed the
// crossings of a line with the previous layer's outline onto it. The vector is reserved for one and a half points
// per input point, so a two-point line that crosses the outline more than twice makes it grow, and every crossing
// after that was filtered against the start of the line read from the freed buffer. Under the address sanitiser
// this case stops with a heap-use-after-free on the sources before the fix.
// Edge: without the sanitiser the freed buffer usually still held the old point, so the case passed before the fix
// too. Inside PoisonFreedMemory the freed buffer reads as NaN, so before the fix the crossings at x = 7 and 9 (those
// tested after the third crossing made the vector grow) fail the spacing filter and are dropped: 3 crossings, not 5.
TEST_CASE("A line crossing the outline five times keeps every crossing", "[ExtrusionProcessor][Regression]")
{
    const AABBTreeLines::LinesDistancer<Linef> prev_layer = comb_outline();
    const Points line{Point::new_scale(0., 0.), Point::new_scale(10., 0.)};

    std::vector<ExtendedPoint> points;
    {
        PoisonFreedMemory poison;
        points = estimate_points_properties<true, true, true, true>(line, prev_layer, caged_wall_width);
    }

    // Each crossing is added at the outline, where the reading is the boundary offset of half a line width.
    std::vector<double> crossings;
    for (const ExtendedPoint &point : points)
        if (std::abs(point.position.y()) < EPSILON && std::abs(point.distance - 0.5 * caged_wall_width) < 1e-4)
            crossings.push_back(point.position.x());
    std::sort(crossings.begin(), crossings.end());

    REQUIRE(crossings.size() == 5);
    for (size_t i = 0; i < crossings.size(); ++i)
        CHECK_THAT(crossings[i], Catch::Matchers::WithinAbs(1. + 2. * double(i), 1e-6));
}

// After an overhang the first move back at the wall's own speed was written at the speed the overhang table
// rounds to whole mm/s (here 162 mm/s, F9720), and only the move after it at the wall's speed (F9744): the test
// for "the speed changed by more than 1 mm/s" came before the test for "the speed is back within 1 mm/s of the
// wall's". One F word more per overhang, and a move printed a little slow.
TEST_CASE("The first move back at the wall speed after an overhang is written at that speed", "[ExtrusionProcessor][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    INFO("wall generator: " << wall_generator);

    constexpr double wall_speed = 162.4; // mm/s, not a whole number: the overhang table rounds it to 162
    Print        print;
    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "caged_overhang.stl";
    object->add_volume(caged_overhang_mesh());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.apply(caged_overhang_config(wall_generator));
    config.set_deserialize_strict({{"outer_wall_speed", wall_speed}});
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    const std::string gcode_out = gcode(print);

    const std::vector<double> feed_rates =
        outer_wall_feed_rates(gcode_out, [](const GCodeReader &, const GCodeReader::GCodeLine &) { return true; });
    size_t at_wall_speed = 0, slowed = 0, rounded = 0;
    for (const double f : feed_rates) {
        if (std::abs(f - wall_speed * MM_PER_MIN) < 0.01)
            ++at_wall_speed;
        else if (std::abs(f - wall_speed * MM_PER_MIN) <= MM_PER_MIN)
            ++rounded; // within 1 mm/s of the wall speed but not it
        else if (f < wall_speed * MM_PER_MIN)
            ++slowed;
    }
    INFO(feed_rates.size() << " outer wall moves: " << at_wall_speed << " at the wall speed, " << slowed << " slowed, "
                           << rounded << " within 1 mm/s of the wall speed but not at it");
    REQUIRE(at_wall_speed > 0);
    REQUIRE(slowed > 0);
    CHECK(rounded == 0);
}

// With "Thick bridges" off an overhang wall is laid with the bridge flow ratio applied to a wall's flow. The classic
// walls had it; Arachne's lost it, being laid at their own width with the plain wall flow, so "Bridge flow ratio"
// did nothing to an overhang wall with Arachne walls (the default).
TEST_CASE("Arachne overhang walls are laid with the bridge flow ratio", "[ExtrusionProcessor][Regression]")
{
    const char *wall_generator = GENERATE("classic", "arachne");
    const char *bridge_flow    = GENERATE("1", "0.8");
    INFO("wall generator: " << wall_generator << ", bridge flow ratio " << bridge_flow);

    // A 10 x 10 mm pillar 4 mm high carrying a 30 x 10 mm slab 2 mm thick, which runs 20 mm out over air.
    indexed_triangle_set part = its_make_cube(10., 10., 4.);
    indexed_triangle_set slab = its_make_cube(30., 10., 2.);
    for (stl_vertex &v : slab.vertices)
        v.z() += 4.f;
    its_merge(part, slab);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_diameter", "0.4"},
        {"layer_height", "0.2"},
        {"initial_layer_print_height", "0.2"},
        {"outer_wall_line_width", "0.45"},
        {"inner_wall_line_width", "0.45"},
        {"wall_loops", "2"},
        {"wall_generator", wall_generator},
        {"bridge_flow", bridge_flow},
        {"thick_bridges", "0"},
        {"enable_support", "0"},
        {"use_relative_e_distances", "1"},
    });
    const std::string gcode_out = Slic3r::Test::slice({TriangleMesh(part)}, config);

    // Filament per mm of the outer and the overhang walls of the slab's first layer.
    double outer_e = 0., outer_length = 0., overhang_e = 0., overhang_length = 0.;
    std::string type;
    GCodeReader parser;
    parser.parse_buffer(gcode_out, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        if (comment.find("FEATURE:") != std::string_view::npos || comment.find("TYPE:") != std::string_view::npos)
            type = std::string(comment);
        if (line.extruding(self) && std::abs(self.z() - 4.2) < 0.01 && line.dist_XY(self) > 0.5) {
            if (type.find("Overhang wall") != std::string::npos) {
                overhang_e += line.dist_E(self);
                overhang_length += line.dist_XY(self);
            } else if (type.find("Outer wall") != std::string::npos) {
                outer_e += line.dist_E(self);
                outer_length += line.dist_XY(self);
            }
        }
    });
    INFO("outer wall " << outer_length << " mm, overhang wall " << overhang_length << " mm");
    REQUIRE(outer_length > 10.);
    REQUIRE(overhang_length > 40.);
    // The ratio applies to the cross section, the walls being as wide as each other here.
    CHECK_THAT((overhang_e / overhang_length) / (outer_e / outer_length), Catch::Matchers::WithinRel(std::stod(bridge_flow), 0.01));
}

namespace {

ModelObject *model_object_named(Model &model, const std::string &name)
{
    for (ModelObject *object : model.objects)
        if (object->name == name)
            return object;
    return nullptr;
}

PrintObject *print_object_named(Print &print, const std::string &name)
{
    for (PrintObject *object : print.objects_mutable())
        if (object->model_object()->name == name)
            return object;
    return nullptr;
}

// The curled edges that slow a wall down come from the layer below it too, not from a layer prepared before.
TEST_CASE("Walls are slowed for the curled edges of the layer below them whichever layer was prepared before",
          "[ExtrusionProcessor][Regression]")
{
    // Nothing, a stale layer, or the stale layer and then the layer below.
    const int prepared_before = GENERATE(0, 1, 2);
    CAPTURE(prepared_before);
    Print print;
    Model model;
    ModelObject *model_object = model.add_object();
    model_object->name        = "cube";
    model_object->add_volume(mesh(TestMesh::cube_20x20x20));
    model_object->add_instance();
    model_object->ensure_on_bed();
    print.auto_assign_extruders(model_object);
    print.apply(model, DynamicPrintConfig::full_print_config());
    PrintObject *object = print_object_named(print, "cube");
    REQUIRE(object != nullptr);
    Layer *stale = object->add_layer(0, 0.2, 0.2, 0.1);
    Layer *below = object->add_layer(1, 0.2, 0.4, 0.3);
    Layer *layer = object->add_layer(2, 0.2, 0.6, 0.5);
    below->lower_layer = stale;
    layer->lower_layer = below;
    // The 0.4mm wide wall runs 0.2mm inside the edge of the layer below, so it is fully supported, but along an edge
    // that curled up 1mm. The stale layer ends 0.8mm short of the wall and has no curled edges: measured against it,
    // the wall would be a full overhang.
    below->lslices      = {ExPolygon(Polygon{Point::new_scale(0, 0), Point::new_scale(40, 0),
                                             Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    below->curled_lines = {CurledLine(Point::new_scale(1, 0.2), Point::new_scale(39, 0.2), 1.f)};
    stale->lslices      = {ExPolygon(Polygon{Point::new_scale(0, 1), Point::new_scale(40, 1),
                                             Point::new_scale(40, 10), Point::new_scale(0, 10)})};
    ExtrusionPath wall(erExternalPerimeter, 0.08, 0.4f, 0.2f);
    wall.polyline.points = {Point::new_scale(1, 0.2), Point::new_scale(39, 0.2)};

    ExtrusionQualityEstimator estimator;
    estimator.set_current_object(object);
    if (prepared_before >= 1)
        estimator.prepare_for_new_layer(object, stale);
    if (prepared_before >= 2)
        estimator.prepare_for_new_layer(object, below);
    estimator.prepare_for_new_layer(object, layer);
    // 100mm/s when fully supported, 20mm/s when a whole width out.
    const std::vector<ProcessedPoint> points = estimator.estimate_extrusion_quality(wall,
        ConfigOptionPercents({100, 0}),
        ConfigOptionFloatsOrPercents({FloatOrPercent{100, false}, FloatOrPercent{20, false}}), 100.f, 100.f, true);

    // A 1mm curl under a 0.2mm layer reads as half the width, 0.2mm, out: halfway between the two speeds.
    REQUIRE_FALSE(points.empty());
    for (const ProcessedPoint &point : points) {
        CHECK_THAT(point.speed, Catch::Matchers::WithinAbs(60., 0.5));
        CHECK_THAT(point.overlap, Catch::Matchers::WithinAbs(0.5, 1e-3));
    }
}

// A 20 x 10 x 2mm wedge whose front face leans out by one layer height per layer (45 degrees), from y = 2 at the bed
// to y = 0 at the top, while its back face at y = 12 stands vertical and fully supported.
constexpr double wedge_height       = 2.;
constexpr double wedge_base_front_y = 2.;
constexpr double wedge_back_y       = 12.;

TriangleMesh wedge_mesh()
{
    const float length = 20.f, top = float(wedge_height), front = float(wedge_base_front_y), back = float(wedge_back_y);
    return TriangleMesh(
        {
            {0.f, front, 0.f}, {length, front, 0.f}, {length, back, 0.f}, {0.f, back, 0.f},
            {0.f, 0.f, top},   {length, 0.f, top},   {length, back, top},   {0.f, back, top},
        },
        {
            {0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7},
            {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5},
            {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7},
        });
}

// A height range modifier over these heights. Layers are sliced half a layer below their print_z, at 0.1, 0.3, 0.5 and
// so on, so the bounds fall between slice planes: the layers printed at 0.6 and 0.8 take the range's settings.
constexpr double wedge_range_min = 0.4;
constexpr double wedge_range_max = 0.8;

bool in_wedge_range(double print_z)
{
    const double slice_z = print_z - 0.5 * caged_layer_height;
    return slice_z > wedge_range_min && slice_z < wedge_range_max;
}

// The wedge with overhang speed set to `object_speed`, and to `range_speed` by the height range modifier.
// Print::apply copies the Model, so the object is found by name, never by a pointer kept across apply().
std::string wedge_gcode(bool object_speed, bool range_speed)
{
    DynamicPrintConfig config = caged_overhang_config("classic");
    config.set_deserialize_strict({
        {"enable_overhang_speed", object_speed ? "1" : "0"},
        // The overhang fan has every layer prepared for overhang detection, whether overhang speed is on or not.
        {"enable_overhang_bridge_fan", "0"},
    });
    Print print;
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "wedge";
    object->add_volume(wedge_mesh());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    ModelObject *wedge = model_object_named(model, "wedge");
    REQUIRE(wedge != nullptr);
    DynamicPrintConfig range_config;
    // Every layer range must carry a layer_height (see layer_height_profile_from_ranges).
    range_config.set_key_value("layer_height", new ConfigOptionFloat(caged_layer_height));
    range_config.set_deserialize_strict("enable_overhang_speed", range_speed ? "1" : "0");
    wedge->layer_config_ranges[{wedge_range_min, wedge_range_max}].assign_config(std::move(range_config));
    print.apply(model, config);
    print.validate();
    print.set_status_silent();
    return gcode(print);
}

// Feed rates of the outer wall moves printed at print_z along the wedge's sloped front face, or along its back face.
// The side walls run from one face to the other, so neither filter takes them.
std::vector<double> wedge_wall_feed_rates(const std::string &gcode, double print_z, bool front)
{
    return outer_wall_feed_rates(gcode, [print_z, front](const GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const auto on_face = [front](double y) { return front ? y < wedge_base_front_y + 1. : y > wedge_back_y - 1.; };
        return std::abs(line.new_Z(self) - print_z) < 0.5 * caged_layer_height && on_face(self.y()) &&
            on_face(line.new_Y(self));
    });
}

} // namespace

// Regression: the estimator measured each layer against the layer it was prepared for before. process_layer() skips the
// layers with overhang speed off, so the first layer above a range turning it off was measured against the layer below
// the range, and the first layer of a range turning it on against no layer at all, which slowed even supported walls.
TEST_CASE("Walls printed with overhang speed on match a print with it always on when a height range toggles it",
          "[ExtrusionProcessor][Regression]")
{
    const bool object_speed = GENERATE(true, false);
    INFO("overhang speed " << (object_speed ? "on" : "off") << " for the object, " << (object_speed ? "off" : "on")
                           << " for the range");
    const std::string toggled   = wedge_gcode(object_speed, !object_speed);
    const std::string always_on = wedge_gcode(true, true);

    // The first layers printed with the other setting, at either end of the range.
    for (const double print_z : {wedge_range_min + caged_layer_height, wedge_range_max + caged_layer_height}) {
        INFO("back wall at z = " << print_z);
        const std::vector<double> feed_rates = wedge_wall_feed_rates(toggled, print_z, false);
        REQUIRE_FALSE(feed_rates.empty());
        for (const double feed_rate : feed_rates)
            CHECK_THAT(feed_rate / MM_PER_MIN, Catch::Matchers::WithinRel(caged_outer_wall_speed, 0.01));
    }

    // Every layer printed with overhang speed on, above the first, which is never slowed for overhangs.
    const int layer_count = int(std::lround(wedge_height / caged_layer_height));
    for (int layer = 2; layer <= layer_count; ++layer) {
        const double print_z = layer * caged_layer_height;
        if (in_wedge_range(print_z) == object_speed)
            continue;
        INFO("slope at z = " << print_z);
        const std::vector<double> expected = wedge_wall_feed_rates(always_on, print_z, true);
        REQUIRE_FALSE(expected.empty());
        // The slope is slowed down, so matching speeds are not merely the wall speed.
        REQUIRE(*std::max_element(expected.begin(), expected.end()) < caged_slow_speed * MM_PER_MIN);
        const std::vector<double> actual = wedge_wall_feed_rates(toggled, print_z, true);
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i)
            CHECK_THAT(actual[i], Catch::Matchers::WithinAbs(expected[i], 1.));
    }
}

// Regression: process_layer() prepared the estimator only when overhang speed was on for the nozzle active before the
// layer's tool changes. Here the second nozzle prints the walls and slows down for overhangs, the first prints the
// infill and has overhang speed off. A layer starts with the nozzle that ended the layer below, so every other layer
// starts with the first one and was not prepared: its walls were measured against an older layer, or against none.
TEST_CASE("Walls printed by a nozzle with overhang speed on match a print with it on for every nozzle",
          "[ExtrusionProcessor][Regression]")
{
    const auto two_nozzle_gcode = [](const char *overhang_speed) {
        DynamicPrintConfig config = caged_overhang_config("classic");
        config.set_num_extruders(2);
        config.set_num_filaments(2);
        config.option<ConfigOptionFloats>("filament_diameter")->values                   = {1.75, 1.75};
        config.option<ConfigOptionStrings>("filament_colour")->values                    = {"#FF0000", "#00FF00"};
        config.option<ConfigOptionFloats>("nozzle_diameter")->values                     = {0.4, 0.4};
        config.option<ConfigOptionInts>("printer_extruder_id", true)->values             = {1, 2};
        config.option<ConfigOptionStrings>("printer_extruder_variant", true)->values     = {"Direct Drive Standard",
                                                                                           "Direct Drive Standard"};
        config.option<ConfigOptionInts>("print_extruder_id", true)->values               = {1, 2};
        config.option<ConfigOptionStrings>("print_extruder_variant", true)->values       = {"Direct Drive Standard",
                                                                                           "Direct Drive Standard"};
        config.option<ConfigOptionBool>("process_extruder_values", true)->value          = true;
        config.option<ConfigOptionBool>("single_extruder_multi_material", true)->value   = false;
        if (auto *flush = config.option<ConfigOptionFloats>("flush_volumes_matrix"))
            if (flush->values.size() < 4)
                flush->values.assign(4, 0.);
        config.set_deserialize_strict({
            {"nozzle_diameter", "0.4,0.4"},
            {"filament_map_mode", "Manual"},
            {"filament_map", "1,2"},
            {"enable_prime_tower", 0},
            {"enable_overhang_speed", overhang_speed},
            // The overhang fan has every layer prepared for overhang detection, whether overhang speed is on or not.
            {"enable_overhang_bridge_fan", "0"},
            {"wall_filament", 2},
            {"outer_wall_filament", 2},
            {"sparse_infill_filament", 1},
            {"solid_infill_filament", 1},
            {"top_surface_filament", 1},
            {"bottom_surface_filament", 1},
            {"skirt_loops", 0},
            {"brim_type", "no_brim"},
        });
        Print print;
        Model model;
        ModelObject *object = model.add_object();
        object->name        = "wedge";
        object->add_volume(wedge_mesh());
        object->add_instance();
        object->ensure_on_bed();
        print.auto_assign_extruders(object);
        REQUIRE(model_object_named(model, "wedge") != nullptr);
        print.apply(model, config);
        print.validate();
        print.set_status_silent();
        return gcode(print);
    };
    const std::string mixed     = two_nozzle_gcode("0,1");
    const std::string always_on = two_nozzle_gcode("1,1");

    // The regression shows only on the layers process_layer() starts with the first filament (T0), on the first
    // nozzle. Make sure the checked layers include some: the tool order, not this test, decides which layers those are.
    int layers_starting_with_first_nozzle = 0;
    std::istringstream lines(mixed);
    int tool = -1;
    for (std::string line; std::getline(lines, line);) {
        if (line.size() > 1 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1])))
            tool = std::stoi(line.substr(1));
        else if (tool == 0 && line.compare(0, 3, ";Z:") == 0 && std::stod(line.substr(3)) > 1.5 * caged_layer_height)
            ++layers_starting_with_first_nozzle;
    }
    REQUIRE(layers_starting_with_first_nozzle > 0);

    // Every layer above the first, which is never slowed for overhangs.
    const int layer_count = int(std::lround(wedge_height / caged_layer_height));
    for (int layer = 2; layer <= layer_count; ++layer) {
        const double print_z = layer * caged_layer_height;
        INFO("z = " << print_z);
        // The supported back wall keeps the wall speed, so the print with overhang speed on for every nozzle measures
        // each layer against a layer below, not against none, which would slow both prints alike.
        const std::vector<double> back_wall = wedge_wall_feed_rates(always_on, print_z, false);
        REQUIRE_FALSE(back_wall.empty());
        for (const double feed_rate : back_wall)
            CHECK_THAT(feed_rate / MM_PER_MIN, Catch::Matchers::WithinRel(caged_outer_wall_speed, 0.01));
        const std::vector<double> expected = wedge_wall_feed_rates(always_on, print_z, true);
        REQUIRE_FALSE(expected.empty());
        // The slope is slowed down, so matching speeds are not merely the wall speed.
        REQUIRE(*std::max_element(expected.begin(), expected.end()) < caged_slow_speed * MM_PER_MIN);
        const std::vector<double> actual = wedge_wall_feed_rates(mixed, print_z, true);
        REQUIRE(actual.size() == expected.size());
        for (size_t i = 0; i < actual.size(); ++i)
            CHECK_THAT(actual[i], Catch::Matchers::WithinAbs(expected[i], 1.));
    }
}
